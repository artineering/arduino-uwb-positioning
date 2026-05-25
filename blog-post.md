# Finding 12 Things in a Room at Once: A UWB Indoor Positioning System

*Filed under: Tinkering, Robotics, Embedded*

GPS works well outdoors. It will tell you where you are anywhere on the planet to
within a few metres — right up until you walk indoors, at which point it gives up.
A few metres of error is fine when you're trying to find a motorway exit. It
doesn't help when you want to know which *desk* something is sitting on, or
whether the robot is on this side of the doorway or that one.

I wanted indoor positioning that was good to a handful of centimetres, for a
roomful of moving things at the same time. The approach I settled on was
**Ultra-Wideband** — the same radio tech that lives inside modern phones and car
keys — and a pile of Arduino boards. This post walks through how I got **4
anchors tracking 12 tags** in real time, and the design decisions along the way.
I'm writing it down mostly to share what I learned, including the parts I'd flag
as limitations rather than features.

## Why UWB and not the usual suspects

The obvious cheap options for "where is this thing" are Wi-Fi RSSI and Bluetooth
RSSI — you measure how strong the signal is and pretend that maps cleanly to
distance. It does not. Signal strength bounces off walls, gets absorbed by
people, and generally lies to you. You end up with positioning that's accurate
to "somewhere in this half of the building."

UWB doesn't measure signal strength. It measures **time** — specifically, how
long a radio pulse takes to fly from one chip to another. Because the pulse is
very short (that's the "wide band" part), you can timestamp it precisely, and
time-of-flight times the speed of light gives you a distance that's good to a few
centimetres. Multilateration handles the rest.

So the plan:

- A few **anchors** bolted to the walls in known positions.
- A swarm of **tags** on the things I want to track.
- Each tag measures its distance to each anchor.
- Some downstream brain turns those distances into an (x, y) coordinate.

Simple in theory. The fun is always in the details.

## The hardware

| Device | Board | UWB Module | Role |
|--------|-------|------------|------|
| Anchor | Arduino Portenta C33 + UWB Shield | Truesense DCU150 (SR150) | Controller (one-to-many) |
| Tag | Arduino Stella | Truesense DCU040 (nRF52840) | Responder |

The anchors are Portenta C33s wearing a UWB shield — beefy boards that sit on
the wall, plugged into power, and don't have to worry about battery life. The
tags are Arduino Stella boards: tiny, nRF52840-based, with UWB and Bluetooth on
the same chip. The Stella is the thing you actually stick on a moving object, so
small-and-low-power matters there.

Both modules come from Truesense, and the heavy lifting is done by their
`PortentaUWBShield` and `StellaUWB` libraries, which wrap the UWB stack into
something an Arduino sketch can talk to.

## Two-Way Ranging, and why I went multicast

The core measurement is **Two-Way Ranging (TWR)**. One device sends a message,
the other replies, and by measuring the round-trip time (and subtracting the
known processing delay on the far end), both sides can compute the distance
between them. No synchronised clocks required, which helps, since synchronising
clocks across a dozen battery-powered boards would be a problem in its own right.

The straightforward version is one anchor talking to one tag at a time. With 4
anchors and 12 tags that's 48 conversations run one after another, and the update
rate drops accordingly. Waiting a couple of seconds to find out where things
moved isn't much use for tracking.

The SR150 supports **one-to-many multicast ranging**: a single anchor runs *one*
session and ranges to a whole list of tags inside it. One broadcast poll goes
out, every tag in the session replies in its own time slot, and the anchor
collects all the distances from one round. That cuts down the overhead compared
to a pile of one-on-one exchanges.

## The session-budget puzzle

Here's the first real constraint, and the thing that shaped the whole addressing
scheme: **the radios have a hard limit of 5 simultaneous sessions**, and a
multicast session can hold at most **8 responders**.

Do the arithmetic from the tag's point of view. Each tag needs to range against
all 4 anchors, so it's in 4 sessions. That's under the limit of 5 — good, with
one to spare.

From the anchor's side, it needs to reach all 12 tags. Twelve is more than the
8-responder cap on a single multicast session, so I split the tags into two
groups of six:

- **Group 0:** Tags 1–6
- **Group 1:** Tags 7–12

Each anchor therefore runs **2 sessions** (one per group), within the 5-session
budget. Four anchors × two groups, and everything fits inside the limits.

To keep this all coherent without a central coordinator, I baked the topology
straight into the MAC addresses and session IDs:

```
Anchor MAC   : { 0xA0, ANCHOR_ID }      ANCHOR_ID ∈ [0..3]
Tag    MAC   : { 0x00, TAG_ID }         TAG_ID    ∈ [1..12]
Session ID   : ANCHOR_ID * 100 + GROUP_INDEX + 1
```

The session ID does some useful work here. It isn't an opaque number — it's a
little self-describing piece of information. When a ranging callback fires, the
anchor firmware can read the session ID and work out which group it's talking to:

```cpp
int groupIndex = (int)(sessionHandle % 100) - 1;   // 0 or 1
int tagBase    = groupIndex * TAGS_PER_GROUP + 1;   // first TAG_ID in group
```

And the tag does the inverse to recover which *anchor* a measurement came from:

```cpp
int anchorId = (int)(sessionHandle / 100);
```

Divide by 100 to get the anchor, mod 100 to get the group. The addressing scheme
encodes the network layout in a single integer, so every board runs the same
sketch, changes *one* `#define`, and knows where it sits in the network.

## Don't compute position on the tag

My first version put the maths in the wrong place. The tag gathered its four
distances, ran the multilateration itself — including a height correction,
because the tags and anchors aren't all at the same elevation — and broadcast a
finished (x, y) coordinate.

It worked. And then I deleted all of it.

The problem is that the moment a tag computes its own position, it has to *know
where the anchors are*. The anchor coordinates are now baked into the tag
firmware. Move an anchor 30 cm to the left, or set the whole rig up in a
different room, and you're reflashing twelve boards. The tag firmware became
tightly coupled to the physical geometry of one specific room, which is exactly
the kind of coupling that turns a fun project into a chore.

So I flipped it. **The tag does no geometry at all.** It just measures its raw
distances to A0, A1, A2, A3 and broadcasts those four numbers. Position
computation is deferred to whatever is listening — a laptop, a Raspberry Pi, a
phone — which is the one place that actually *should* hold the map of where the
anchors live.

The payoff:

- Tag firmware is identical regardless of room layout. One sketch, one `#define`.
- Rearranging the anchors is a config change on *one* receiver, not a reflash of
  the whole swarm.
- The receiver has a real CPU, a screen, and more floating-point headroom for
  solving the multilateration than an nRF52840 doing trig in an interrupt handler.

The general idea, which I keep coming back to: put state and policy on the part of
the system that's best placed to hold it. The tag's job is to measure. The
geometry can live wherever the map of the room already lives.

## Broadcasting distances over Bluetooth

Since the tags already have Bluetooth on the same chip, I didn't bother pairing
or building a connection. The tags just *shout* their distances into the air as
BLE advertising packets — connectionless, no handshake, and any number of
receivers can listen at once.

I packed everything into an 11-byte manufacturer-data blob:

| Byte | Content |
|------|---------|
| 0–1 | Company ID `0xFFFF` (the BLE SIG test/dev ID, little-endian) |
| 2 | TAG_ID |
| 3–4 | Distance to A0 in cm (`uint16`, little-endian) |
| 5–6 | Distance to A1 in cm |
| 7–8 | Distance to A2 in cm |
| 9–10 | Distance to A3 in cm |

A couple of small touches that matter more than they look:

**`0xFFFF` means "I don't know."** If a tag hasn't heard from an anchor recently
— it's out of range, or behind a filing cabinet — the distance for that anchor is
reported as `0xFFFF` rather than a stale lie. Anything older than 2.5 seconds is
declared invalid and flagged this way. A receiver would much rather be told "I
have no idea where A2 is" than be handed a confident, wrong number from ten
seconds ago.

**Only re-advertise when something actually changes.** Re-broadcasting the same
four numbers forty times a second is just RF noise and wasted battery. The
firmware tracks the previously-advertised values and only pushes a new packet
when a distance genuinely moves, with a minimum interval between updates to keep
the radio from thrashing. Quiet when there's nothing to say.

## Getting to 4 Hz

The last meaningful tuning step was update rate. Early on, the ranging duration
was set conservatively and positions trickled in slowly enough to feel laggy
when something moved. Dropping the `rangingDuration` to 250 ms gets each session
ranging at **4 Hz** — four fresh fixes per second per tag — which was enough for
tracking people and slow-moving robots without flooding the radio or starving the
other sessions sharing the medium.

```cpp
sessions[g]->appParams.rangingDuration(250);   // 4 Hz
```

There's a real tension here: faster ranging means fresher data but more airtime
contention between all the sessions sharing the same physical channel. 4 Hz, in
practice, was the point where it stayed responsive without the sessions starving
each other.

## The big caveat: this only works with a fixed topology

It's worth being clear about what this design does *not* handle, because it's the
assumption everything above quietly rests on. The whole scheme works because the
roster is **fixed and known at flash time**. There are exactly 4 anchors and
exactly 12 tags, the group assignments are decided in advance, and the session
budgets are worked out on paper before a single board is powered on. Every board
is told its place in the network through one `#define`, and nothing changes after
that.

That's fine for a fixed installation, but it sidesteps a much harder problem:
**tags that join and drop off on demand.** As soon as the set of tags becomes
dynamic, the tidy arithmetic stops being something you can do by hand. Each anchor
would have to track, at runtime, how many sessions it currently has open and how
many controlees are in each one, and then assign each newly arriving tag to a
group without exceeding the 5-session and 8-responder limits. That's a
load-balancing problem running live on every anchor, with all the awkward bits
that come with it — allocating and freeing session slots, coordinating which
anchor owns which tag, and noticing when a tag disappears without telling anyone
so its slot can be reclaimed.

I haven't tackled any of that here, and I don't want to imply the static version
solves it. Dynamic membership is a genuinely more involved system, and treating it
as a small extension of this one would be a mistake. What's in this post is the
fixed-topology case, which is the right place to start but not the finish line.

## Where it lands

The result is a roomful of boards that cooperate: four anchors on the walls
running two multicast sessions each, twelve tags each ranging to all four anchors
and broadcasting their distances to anyone listening, all updating four times a
second. Point a BLE scanner at the room and you get a live stream of `UWB-T1`
through `UWB-T12`, each one reporting how far it is from each corner. Feed that
into a multilateration solver and you have centimetre-level indoor positioning
for a dozen things at once.

For a fixed set of anchors and tags, keeping the topology in the addressing math
and the geometry at the receiver kept the firmware simple, and the same sketch
runs on every board with one number changed. The dynamic-membership case is the
obvious next thing to work out, and it's a fair bit harder.

The firmware is on GitHub under Apache 2.0, in case any of it is useful to you.

---

*Make, Create, Share!*

# Vigiles — a guided tour

> *Ubi fumus, ibi ignis*

This is a small Windows application that answers a question most of us have
asked while watching a hard drive light blink for no obvious reason: *what is
actually touching my disk right now, and where?*

The left half of the window is an ordinary folder tree. The right half is a
live feed of file operations. The interesting part is what happens between
them. Folders light up as work happens inside them, in colours that tell you
what kind of work it was, and the tint fades over about five seconds so you can
follow a burst of activity as it moves through a directory structure. Watching
a compiler run, or Windows Update wake up, becomes something you can see rather
than infer.

What follows is less a manual than an explanation of why the thing is built the
way it is. Most of the decisions had a road not taken, and the roads not taken
are usually the more instructive part.

---

## The first fork: driver or no driver

The obvious reference point here is Process Monitor, which does this job
superbly and does it with its own file system minifilter driver. That is the
maximalist approach, and it buys real capability. A minifilter sits in the I/O
path itself, sees every operation before it completes, and could in principle
block or alter it.

It also costs a great deal. You need an EV code signing certificate, then
Microsoft attestation signing on top of it, or the driver simply will not load
on a normal machine. You need a service to install and start it. You need to be
comfortable with the fact that a null pointer dereference is no longer an
application crash but a bugcheck, and that your debugging loop now involves a
second machine and a kernel debugger.

For a viewer, none of that is necessary. Windows already publishes the same
information through Event Tracing for Windows. The provider called
`Microsoft-Windows-Kernel-File` emits create, read, write, delete, rename,
set-information, cleanup, close and directory enumeration events, and any
elevated user mode process can subscribe to them. The event stream is
essentially what the minifilter would have seen, delivered without the driver.

The rule of thumb that falls out of this: write a driver when you need to
*change* what happens. When you only need to *know* what happened, ETW is
already there and it is free.

The one thing we give up is physical disk behaviour. Kernel-File describes file
system activity, not spindles and queues. If you want per-disk throughput and
queue depth, that lives in `Microsoft-Windows-Kernel-Disk`, which can be enabled
in the very same session and correlated on IRP pointer. The plumbing for that
is already in place.

---

## The buffering problem, which is the real problem

The original request included a phrase that turned out to be the crux of the
whole design: *with enough buffer to display all the activity*. That sounds
like a single setting. It is actually two separate buffers that fail in two
different ways, and conflating them is how monitoring tools end up quietly
lying to you.

**The kernel buffers** come first. When you start an ETW session you tell the
kernel how much memory to set aside for events that have been generated but not
yet consumed. Here that is 512 buffers of 64 KB, so 32 MB, with a one second
flush timer. If the consumer falls behind for longer than that reservoir lasts,
the kernel does the only thing it sensibly can: it drops events and increments
a counter. That counter is surfaced through the buffer callback and displayed
in the status bar as "Kernel lost". If you ever see that number move, the fix
is to raise `maxBuffers` in `EtwConfig`.

**The in-process ring** comes second. Events that survive the trip out of the
kernel land in a fixed ring of one million records. Each record is 32 bytes,
because paths are interned to an integer id rather than stored inline, so a
million records costs about 32 MB rather than several hundred. The ring is
drop-oldest: under a storm, the newest events push the oldest out.

Now the important bit, and the one design decision I would defend hardest.
**Aggregation happens on the ETW thread, before anything reaches the ring.**
When an event is decoded, its folder counters are incremented immediately.
Only then does the record go into the ring for the detail list.

The lazy alternative is to drain the ring on the UI thread and aggregate there.
It is less code, and it is wrong in a specific and nasty way: your per-folder
totals would silently become inaccurate exactly when the disk is busiest, which
is exactly when you are looking at them. By aggregating first, the two failure
modes stay cleanly separated. The tree counters are always exact. The scrolling
list is best-effort, and it tells you when it dropped rows.

---

## Reads and writes do not know their own names

Here is a wrinkle that is not obvious until you look at the raw events. A read
or write event does not carry a file path. It carries a `FileObject` pointer
and a `FileKey`. Paths appear only on create events and on the name rundown
events that the provider emits for files that were already open.

So the consumer maintains two maps, `FileObject -> name` and `FileKey -> name`,
populated from every event that does carry a path, and evicted on close. A read
event resolves its path by looking up its object, falling back to its key. When
neither hits, which happens for handles opened before tracing started, the
event is skipped rather than shown against a wrong path.

To shrink that blind spot, the session issues a `CAPTURE_STATE` call right after
enabling the provider, which asks it to replay its current name table. It is the
difference between the first few seconds being useful and being noise.

One more translation is needed. The kernel talks in device paths like
`\Device\HarddiskVolume3\Windows\System32`. Humans and tree controls talk in
drive letters. A map built at startup from `GetLogicalDriveStrings` plus
`QueryDosDevice` converts one to the other, with a special case for `\Device\Mup`
so that network paths come back as UNC.

---

## Making the hot path cheap

Every event has to be attributed not just to its own folder but to every folder
above it, so that `C:\` shows the sum of everything beneath it. Done naively,
that means splitting a string and hashing it once per ancestor, per event, at
tens of thousands of events per second.

Instead, the ancestor chain is resolved once per interned path and cached. The
first time we see `C:\Users\bob\project\main.cpp`, we walk up and record the
node pointers for `C:\Users\bob\project`, `C:\Users\bob`, `C:\Users` and `C:\`.
Every subsequent event on that file is a handful of pointer increments under a
single mutex. The string work happens once per distinct path, not once per
operation.

The remaining cost is decoding. Properties are currently read by name with
`TdhGetProperty`, which is the convenient path rather than the fast one. It is
comfortable into the low hundreds of thousands of events per second and becomes
the bottleneck beyond that. The upgrade, if you ever need it, is to resolve
property offsets once per event descriptor with `TdhGetEventInformation` and
then parse the raw `UserData` blob directly.

---

## Why the interface behaves the way it does

**The fade is eased, not linear.** The tint lasts five seconds, but it does not
decay evenly. A square root curve holds the colour near full strength for the
first couple of seconds and then drops away. A linear ramp over five seconds
spends most of its life as a barely visible wash, which defeats the purpose.

**Colour encodes the operation, in pastels.** Reads are powder blue, writes
apricot, creates mint, deletes rose, renames lavender. The list rows use light
versions so the text stays legible; the tree uses the same hues at higher
saturation, because they get blended back toward white as the heat decays. The
hue for a folder comes from whichever operation dominated in the last refresh
interval, not from its lifetime totals, so a folder that has been read a million
times still flashes orange the moment something writes to it. When a folder goes
quiet the last hue is retained while it fades, rather than snapping to grey
halfway through.

**Selecting a folder clears the list.** The right pane is a live view with no
history. Changing selection wipes the rows and discards whatever was queued, so
you see only what happens from that moment on. This started as a small usability
fix and turned into the feature that makes the pane worth having. A scrolling
log of everything on the machine is unreadable; a scrolling log of one folder,
starting now, is a diagnostic tool.

The filter that backs it is careful about boundaries, incidentally. A plain
string prefix match on `c:\windows` also matches `c:\windowsapps` and
`c:\windows.old`, so the match requires a path separator immediately after the
prefix, with drive roots special-cased since they already end in one.

---

## Handing the question to something that can answer it

The tool is good at showing you that `C:\Windows\...` is on fire and useless at
telling you why. That last step is a research problem, not a monitoring one, so
the application does not try to solve it: a button collapses what is on screen
into a brief and puts it on the clipboard for whatever assistant you prefer.

The temptation is to dump the captured rows. Resist it. Two thousand lines of
`Read C:\Windows\System32\ntdll.dll` carry exactly as much information as one
line saying it happened two thousand times, and they cost a thousand times the
context. So the brief is four rankings - by process, by subfolder, by file, by
extension - each entry printed once with a count, the set of operations seen on
it, and total bytes. Eight hundred operations compress to under 3 KB, and the
compression is lossless in the only sense that matters: nothing a reader would
conclude from the full list is absent from the counts.

Aggregating surfaced a bug that had been sitting in the display all along.
Windows reports the same file under different spellings - the kernel emits both
`\WINDOWS\SYSTEM32\` and `\Windows\System32\`, occasionally within the same
second - so keying on the raw string split one real file into several entries
and quietly divided its count between them. In the tree that was invisible. In
a ranked list it put the wrong file at the top. The keys are folded to lowercase
now, while the first spelling seen is what gets printed.

This is a recurring shape, worth naming: aggregation is a correctness test for
data you thought you already understood. The counters had been feeding the tree
for days without anyone noticing, because a tint does not care whether it is
driven by one counter or two.

---

## When it goes wrong

The application logs to `Vigiles.log` beside the executable, opened with
`FILE_FLAG_WRITE_THROUGH` so that whatever was written before a crash is
genuinely on disk afterwards. Every line carries a thread id, which matters
because the ETW callback runs on its own thread and its failures look very
different from the UI thread's.

Four handlers cover the ways a GUI process can die without saying anything: an
unhandled exception filter that writes a minidump, a terminate handler for
uncaught C++ exceptions, and the CRT invalid parameter and pure call handlers.
The event callback is wrapped in both a structured and a C++ exception guard,
because an exception escaping into `ProcessTrace` kills the process outright,
with no dialog and no clue.

Two bugs found during bring-up are worth recording, since both are the kind that
reappear.

The first was a lock re-entrancy hazard. The tree refresh held the statistics
mutex while calling `TreeView_SetItem`, and the custom draw handler locked that
same non-recursive mutex on the same thread. The fix was to split the refresh
into phases: gather what is visible, snapshot the counters under the lock,
release it, then touch the control. Custom draw reads the snapshot without
locking anything. The general lesson is that holding a lock across a call into
someone else's code, especially a window procedure that can call you back, is a
trap regardless of how briefly you hold it.

The second was `EventTraceGuid`, which is declared `extern` in `evntrace.h` and
only defined if you set `INITGUID` before the include. Rather than reach for
that, the callback filters positively on the Kernel-File provider GUID, which
is what we actually wanted and discards the session's own header records as a
side effect.

---

## Where you might take it next

Process names are the most obvious gap. The list shows raw PIDs; resolving them
lazily through `OpenProcess` and `QueryFullProcessImageName` with a small cache
would make the pane far more informative, and enabling
`Microsoft-Windows-Kernel-Process` would catch short-lived processes that have
already exited by the time you ask.

Beyond that: operation latency, by adding the `OP_END` keyword and pairing
initiation with completion, at roughly double the event volume. Physical disk
statistics from the Kernel-Disk provider. A legend for the colour palette. And
a tree that notices folders created after you expanded their parent, which today
it does not.

None of these need a driver either.

# Contribution Guidelines

## A note for contributors

Thank you for your contribution!

Please make sure you tick the box labelled `Allow commits from members who can
merge to the target branch`. This allows us to make minor edits ourselves, and
then automatically rebase and merge your changes.

PulseAudio is currently maintained by three volunteer developers in their free
time (probably amounting to less than one full time developer), which is not
really enough, given the project size and scope. For this reason bug reports
and patch submissions are sometimes handled very slowly.

For non-trivial patches, we meet biweekly on IRC to discuss and prioritise
outstanding MRs. If you haven't heard from us a few days after you create the
MR, please take a look at [the patch status
page](https://www.freedesktop.org/wiki/Software/PulseAudio/PatchStatus/).

If you don't see your MR in that list either, please don't hesitate to drop a
comment pinging us, and we'll try to at least respond and make sure your
request is tracked on that list.

## Coding Style

Please take a look at the [coding style
documentation](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/CodingStyle/)
on our wiki.

## Commit messages

We follow the standard git commit message format of a summary on the first line
(<=50 characters for preference, <=72 characters otherwise), followed by a new
line, followed by a detailed commit message. An additional line at the end may
link to an issue being fixed by this MR.

The first line is usually a short description of "what" your commit does, and
the rest of the message describes the "why", along with any additional
information that readers might need to understand the rationale for the change.
If in doubt, more verbose is better than less.

If you need to describe the "how" of the commit, that is usually best
documented along with the code itself.

Commit messages are prefixed with the subsystem being affected. Your best bet
to figure out what is appropriate is to look at previous commit messages. An
example:

```
sink: Reduce chat sink priority

Some gaming sound cards have custom profiles with analog-game and
analog-chat mappings that exist simultaneously. The game sink should
have higher priority than the chat sink, but currently there's no way to
affect the sink priorities from the profile-set configuration (the
mapping priority is not propagated to the sink priority).

I first thought about adding the mapping priority to the sink priority,
but that could mess up the prioritization system in
pa_device_init_priority(). I ended up checking for the intended roles
property to reduce the chat sink priority. I also reduced the iec958
priority so that the chat and iec958 sinks don't end up with the same
priority.

Fixes: https://gitlab.freedesktop.org/pulseaudio/pulseaudio/issues/818
```

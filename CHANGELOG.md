# What changed

## 1.1.0 — 12 October 2026

**A real desktop app, no browser.**
There is now a program you click, not a web page: a normal window, the same shape as other GPU tools
on Linux. It shows your cards and what each one can actually do, a button to give your other cards a
job, the `doctor` check, and the on/off switch for the whole thing. The installer builds it for you.

**An icon.**
One card feeding two others through a visible wire — the card you play on in light blue, the two taking
work in amber. It ships at every normal size so it looks right in the menu, on the desktop and in the
task bar.

**The tool now says what each card can do.**
`spirit probe --json` gained three fields per card: `va_driver`, `can_encode`, and `marginal`. Before
this, both the app and the page worked out "can this card take a job?" for themselves, from the driver
name and a hardcoded number. That kind of second guess is how a screen ends up promising a job the tool
would refuse. Now the answer comes from the tool.

**The tool no longer disagrees with itself about which card your monitor is on.**
The game path picked the main card by "is a cable plugged into it", while `doctor` used "is it actually
driving a picture". Those are different questions, and on the wrong machine they gave different answers —
so the game could be pinned to the wrong card, and the real one could be handed a job. Both now use the
active output, then the connected one, then the fastest.

**Fixes**
- A job split across cards now hands back **one file**, not a pile of pieces. The slices are joined and
  the result is checked against the original.
- Nothing starts on a card with a narrow connection to the machine unless you name that card. Before,
  the rule only excluded connections narrower than four lanes, which let a four-lane card through — and
  four lanes is exactly what a docked card looks like.
- The installer no longer overwrites your `hicolor/index.theme`. Earlier in this release it did, which
  would have quietly degraded icons **for every app on your machine**. It ships the pictures, not the
  index.
- Installing the desktop shortcut no longer needs a command line.

## 1.0.0 — 11 October 2026

First release. Three builds of the same tool (Python, C++, Rust) held to identical output by two test
suites; a real job on the spare cards while you play; `--split` to divide one job across cards.

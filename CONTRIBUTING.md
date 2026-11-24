# Contributing to MESHtop

MESHtop is a community project. Ideas, bug reports and improvements are all welcome.

## Reporting a bug
Open an issue and include:
- the MESHtop version (on the STATUS screen, or the first lines of the serial log),
- the board's hardware version (V1.3, V1.4 or V1.5, on the label),
- what you did, what you expected, and what happened,
- the serial log if you can (USB-C, 115200 baud). **Remove your Wi-Fi name, location and anything else
  private before posting it.**

## Sending a change
1. Fork the repository and make a branch.
2. Build with PlatformIO (`pio run -e crowpanel7`; see the README) and run the simulator tests
   (`make -C sim -j8 && ./sim/build/scopething-sim`). They should end with `SELF-CHECK OK`.
3. Keep the privacy rule: **nothing from the user's own radio may be sent to CoreScope or anywhere else on the
   network.** `tools/privacy_check.py` runs on every build and will stop it if radio code gains a network path.
4. Keep the settings compatible: the `scopething` settings namespace and the `/scopething` SD folder stay, so
   updates keep everyone's settings.
5. If you use someone else's work, add it to `CREDITS.md` with its licence. Don't copy code from GPL projects into
   MESHtop; it's MIT-licensed.
6. Add a line to `CHANGELOG.md` and open a pull request describing what changed and how you tested it.

By contributing you agree your contribution is released under the MIT License.

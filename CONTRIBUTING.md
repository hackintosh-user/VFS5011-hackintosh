# Contributing to Hackintosh TouchID

Contributions are welcome and genuinely help grow this project. There are two main ways to help: testing, and code. 

See [SECURITY.md](./SECURITY.md) if what you have is a security report rather than a bug or feature contribution — please don't file those as a regular issue or PR, As this project is a biometric autnetication and it's a security matter!

## Testing

Testing a fingerprint sensor currently in development is the best way to make sure new code ships properly, with no bugs or security issues. Testers are heavily welcomed.

To apply for testing a fingerprint sensor in development, send the following (see [Contacting Me](#contacting-me) below):

* Your laptop model
* Your macOS version (must meet the project's minimum)
* Your OpenCore version (must be v1.0.6 or later)
* Your fingerprint sensor
* Your VID:PID — **without this I can't use your help**
* Your timezone, so I don't ping you at 4AM (mine is UTC+3, KSA time)
* Whether your sensor is even enabled in your USBMap / USBMap.kext — this determines if the sensor gets powered at all. Check your USBMap, then check System Information → USB for your sensor's VID:PID.

## Helping with Code

Also heavily welcomed — this is what rapidly expands the project's sensor support and helps more people.

* This project is written in C, and its fingerprint-sensor code is based on libfprint/fprintd conventions. If you know C, you're welcome to contribute. Anyone's welcome to try regardless.
* Before opening a PR that adds a new sensor backend or touches the build, check the build scripts (`build_*.sh`) and CI config (`ci.yml`) — a new source file needs to be added in all of the places that reference the daemon's source list, or CI will pass green while the actual build is broken.
* Open an issue first for anything nontrivial (new backend, architecture change) so we can align before you put in the work. Small fixes/typos can just be a PR.


## Contacting me

* [Reddit](https://www.reddit.com/user/Sufficient_Bus_8302/)
* [XDA Forums](https://xdaforums.com/m/hackintosh_user.13447103/)
* [Mac Rumors Forums](https://forums.macrumors.com/members/hackintosh_user.1420199/)
* [𝕏 or Twitter]( https://x.com/mohammad_q_124) (X Chat or idk what it's called)


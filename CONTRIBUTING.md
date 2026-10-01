# Contributing

## Issues

Bug reports, questions and requests are welcome in the issue tracker of any SDK repository. If
you are not sure which repository an issue belongs to, open it in
[oni-sdk-docs](https://github.com/Salacious-Oni-Dev/oni-sdk-docs/issues).
Security problems are the exception: see [Security](#security) below.

A useful bug report gives the SDK release you built, the game build, whether you were
reading a recorded corpus or a running game, the exact command line, and the program's output.
Do not attach a corpus: it contains data from the game and must not be redistributed. The issue
template asks for each of these.

## Security

Please do not report a security problem in a public issue. Use GitHub's private vulnerability
reporting instead: open this repository's **Security** tab and choose **Report a
vulnerability**. The report is visible only to you and the maintainers, and the fix is
discussed there before anything is made public. If you are unsure which repository a problem
belongs to, report it in any of them.

A useful report says which release and game build you used, what an attacker needs (a file
you open, access to your machine, access to your network), what they can do with it, and how
to reproduce it. During the alpha only the latest release is supported, so check that the
problem is still there in it.

Some behaviour is documented and intended, such as a development tool that listens on the
network without authentication. That is not a vulnerability on its own. It is one if it
happens when the documentation says it does not, or reaches further than the documentation
says.

**In scope here:** `simviz.exe` reading a corpus, a state dump or any other file it is given,
and the `--attach` connection: what it does with the responses from the host and port it is
pointed at. It loads the `SimDLL.dll` it is told to load and runs it, so pointing it at an
untrusted library is not a vulnerability. The debug server it attaches to belongs to
oni-framework-api.

## Pull requests are not accepted yet

During the alpha, the public repositories are generated from the development repositories at
each release. A commit made here would be replaced by the next release, so pull requests
cannot be merged.

If you have a fix, describe it in an issue, with a patch or a snippet if it helps. Fixes are
ported by hand, and the changelog entry for the fix links the issue.

## Licensing

A patch or snippet posted in an issue is taken as offered under this repository's license,
the Mozilla Public License 2.0.

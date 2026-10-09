# Contributing to neutron

Thanks for helping. Bug reports, game tests and fixes are all welcome.

## Reporting a game

Open an issue with the bug template. Attach `~/Library/Logs/neutron/neutron-<appid>.log`
and `~/Library/Logs/neutron/steam-hook.log`, and say which neutron commit, macOS
version and Mac you use. "Works" reports help too (which settings, how it runs).

## Workflow

1. Fork the repo (or, with write access, create a branch).
2. Branch from `main`: `feat/...`, `fix/...`, `perf/...` or `docs/...`.
3. Commit with a sign-off (`git commit -s`, see below).
4. Open a pull request to `main`. Say what you changed, why, and how you tested it
   (game, appid, `tests/` program, benchmark numbers before and after).

`main` only changes through pull requests; they are merged as one squash commit.

## AI tools

Using AI tools is fine, neutron itself was mostly written with AI (see the README).
Say in the pull request if you used one, and only send changes you understand and
tested. You are responsible for what you submit.

## Developer Certificate of Origin

Every commit needs a `Signed-off-by: Your Name <you@example.com>` line
(`git commit -s` adds it). With it you confirm the
[Developer Certificate of Origin](https://developercertificate.org): you wrote the
change or have the right to submit it under the project's license. There is no
contributor license agreement.

## License of contributions

The repo's own files are under the LGPL-2.1-or-later (`LICENSE`). Patches in
`patches/` keep the license of the project they patch (Wine and DXMT:
LGPL-2.1-or-later, FEX: MIT). Never add code from Valve's Steamworks SDK or Apple
binaries to the repo.

## Patches and code

- Build with `./build.sh`, install your build with `./install.sh --dist <dir>`.
- Patch conventions (how the Wine, DXMT, FEX, Proton and llvm-mingw patches are
  made and applied) are in `CLAUDE.md` under Conventions. Mark changes in upstream
  code with a `/* neutron: ... */` comment that says why.
- Each patch starts with 2 to 4 lines that say what it does and why.
- Test what you can headless with the programs in `tests/` (see `tests/README.md`)
  and the benchmarks in `dev/bench/`; say in the pull request what you ran.
- Scripts: bash 3.2 compatible, `set -euo pipefail`, quote paths.
- Text: plain technical English.

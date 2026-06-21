# Contributing

Thanks for your interest in `3dtiles-viewer`. This is a small, deliberately lean
codebase; the bar for changes is "does it stay simple, portable, and safe on
untrusted input?".

### AI-Assisted Code

AI-generated or AI-assisted code is **accepted**, provided it passes the same quality gates as human-written code. No exceptions.

### Author Identity

Commit signatures must reference a **contactable human**. We accept:

- Real names with a verifiable email
- Pseudonyms with a contactable email (GPG-signed)

We do **not** accept:

- Bot accounts with no human contact
- Anonymous identities with no way to reach the author
- AI model names as authors (GPT-5, Gemini, Grok, Claude, Ollama models, etc.)

If an AI tool assisted your contribution, mention it in the commit body (e.g., `Co-Authored-By: Claude <noreply@anthropic.com>`), but the primary author must be a human we can contact.


## Ground rules

- **Untrusted input is the threat model.** Every `tileset.json` and `.glb` is
  assumed hostile. A malformed or malicious tile must never cause memory
  corruption, an out-of-bounds access, an integer-overflow under-allocation, or
  an escape beyond `http`/`https`. The worst acceptable outcome is a skipped
  tile with a logged reason. New parsing or loading code is held to this bar.
- **Portability over GL features.** The viewer negotiates a GL version ladder
  (desktop core 3.3..4.6, GL ES 3.0) and must keep running on software /
  virtualized stacks (llvmpipe, virgl). Do not hard-require a single GL version
  or an optional extension; probe it through `gpu_caps` and degrade.
- **Keep it lean.** Vendored, header-only third-party deps under `third_party/`;
  no new heavy runtime dependency without a strong reason.

## Style

- C++17. Source is ASCII-only (no non-ASCII characters in code or comments).
- Formatting is enforced by `clang-format` (config in `.clang-format`). CI pins
  **clang-format 19.1.7**; major versions disagree on lambda/brace wrapping, so
  use that release (e.g. `pipx install clang-format==19.1.7`) or your check may
  pass locally yet fail CI. Run it before sending a change:

  ```
  clang-format -i $(find src tests -type f \( -name '*.cxx' -o -name '*.h' \))
  ```

## Building and testing

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CI builds the GCC and Clang matrix with the HTTP loader both on and off, runs the
tests, and checks formatting. Please make sure those pass locally.

## Pull requests

- One logical change per PR; describe the motivation, not just the diff.
- For anything touching the parsers or the loaders, say how you considered the
  untrusted-input threat model.
- Add or extend a unit test when you touch pure logic (e.g. the URI resolver).

## License

By contributing you agree that your contributions are dual-licensed under
Apache-2.0 OR MIT, matching the project.

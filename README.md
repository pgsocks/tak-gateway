This Git template is a starting point for native libraries with
C11-compatible public C interfaces, procedural C++17 implementations, and
procedural C++ headers. It is intended for maintainers who want explicit
ownership, status-based error reporting, documented contracts, and behavior
driven development. [CONTRIBUTING.md](CONTRIBUTING.md) defines the implementation
restrictions and contributor workflow.

The template provides build, test, and CI infrastructure. Product code, unit
cases, and acceptance scenarios start empty. Enabled product suites fail until
you add coverage; separate infrastructure regressions verify the test runner.

# Quickstart

Install CMake 3.24 or newer, Git, and a C/C++ toolchain with C++17 support.
Initial dependency downloads require network access. Run these commands from
the repository root to configure and build the default unit suite:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The build should succeed. In the unchanged template, CTest intentionally fails
with `Unit suite contains no cases`. Add public API tests to make this suite
pass. Ruby and Bundler are unnecessary for this configuration.

Acceptance testing is optional. Install Ruby, Bundler, and `patch` before
running the following commands from the repository root:

```sh
gem install bundler
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Infrastructure regressions should pass. Product suites intentionally fail
with no unit cases and no selected acceptance scenarios. Infrastructure
fixtures do not count as product coverage. See the [testing guide](docs/testing.md)
for prerequisites, focused runs, opt-out presets, and troubleshooting.

Test dependencies are excluded from installation. There is no product to
install yet. After adding installable targets, use `cmake --install build`
for the default build; the shared installation prefix is `$HOME/.local`.
CMake has no install presets.

# Start a project

Use this checklist when creating a repository from the template:

1. Choose the project name. Update `project(...)` in `CMakeLists.txt` and
   replace this README introduction with the product's purpose, audience,
   limitations, and quickstart.
2. Choose supported platforms and toolchains. CI currently exercises Linux
   with GCC and Clang, and Windows with MSVC. Other platforms are unverified.
   Update shared configure, build, and test presets together when adding
   supported configurations.
3. Choose and add a license before distributing the template or derived
   project. This template supplies no project license. Review applicable
   upstream notices when distributing dependencies. [GitHub's licensing
   guide][licensing] explains how to select and include a license.
4. Write the product's [API contract](docs/api.md) and
   [design rationale](docs/design.md). Those documents currently explain how
   to author them; replace the guides with concrete product documentation.
5. Add library targets and public headers, then link tests against those
   targets. Add unit cases and meaningful acceptance scenarios through the
   public interfaces. Keep runner fixtures separate from product coverage.
6. Define versioning, compatibility commitments, and release checks. Update
   the documentation when these decisions change, and verify the project's
   quickstart from its declared prerequisites.

# Documentation

Keep essential contracts, research, and design rationale in the repository so
contributors can understand both intended behavior and the reasons for it.

- [Writing an API contract](docs/api.md)
- [Recording design rationale](docs/design.md)
- [Testing](docs/testing.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, commit guidelines, and
dependency policy.

[licensing]: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository

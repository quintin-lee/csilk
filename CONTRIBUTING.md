# Contributing to csilk

Thank you for your interest in contributing to csilk! We welcome all contributions, from bug reports and documentation improvements to new features and performance optimizations.

## Getting Started

1.  **Fork the repository** on GitHub.
2.  **Clone your fork** locally:
    ```bash
    git clone https://github.com/yourusername/csilk.git
    cd csilk
    ```
3.  **Build the project** and run tests to ensure everything is working:
    ```bash
    mkdir build && cd build
    cmake ..
    make run_tests
    ```

## Coding Standards

- **C11**: Use standard C11 features. Avoid platform-specific extensions unless necessary (and provide fallbacks).
- **Style**: We use `clang-format` to maintain consistent code style. Run `make format` before committing.
- **Memory Safety**: Always check memory allocation results. Use the request-scoped `arena` whenever possible for temporary data.
- **Documentation**: Document all new public APIs and internal functions using Doxygen comments (`/** ... */` style). Public APIs go in `include/csilk.h`, `include/csilk_app.h`, `include/csilk_internal.h`, and `include/csilk_reflect.h` with `@brief`, `@param`, `@return` tags. Implementation files in `src/` should include `@file` and `@brief` at minimum. Use `@copyright MIT License` for license attribution.

## Pull Request Process

1.  **Create a new branch** for your work:
    ```bash
    git checkout -b feature/your-feature-name
    ```
2.  **Implement your changes** and add tests if applicable.
3.  **Verify your changes**:
    - Run `make run_tests` to ensure no regressions.
    - Run `make tidy` to check for performance and static analysis issues.
4.  **Commit your changes** with a clear and concise message.
5.  **Push to your fork** and **open a Pull Request** against the `main` branch.

## Bug Reports

If you find a bug, please open an issue on GitHub with:
- A clear description of the problem.
- Steps to reproduce the issue.
- Expected vs. actual behavior.
- Your environment (OS, compiler version, libuv version).

## License

By contributing to csilk, you agree that your contributions will be licensed under the project's [MIT License](LICENSE).

# csilk Python Bindings

Python bindings for the `csilk` C web framework and AI workflow orchestrator.

## Installation

To install the package in editable development mode:

```bash
pip install -e .
```

> [!NOTE]
> The underlying C library (`libcsilk.so` or `libcsilk.dylib`) must be compiled and available in your library search path or in the root directory as resolved by the ctypes loader.

## Running Tests

To run the unit test suite:

```bash
python3 -m unittest discover -s tests
```

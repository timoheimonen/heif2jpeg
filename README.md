# HEIF2JPEG

A simple utility to convert HEIF/HEIC image files to JPEG format.

## Description

HEIF2JPEG is a command-line tool that allows for easy conversion of High Efficiency Image Format (HEIF/HEIC) files, commonly used by Apple devices, to the more widely compatible JPEG format.

## Features

- Batch conversion of multiple HEIF/HEIC files
- Preserves image quality during conversion
- Maintains image metadata (Exif, XMP, IPTC)
- Multi-threaded processing for better performance
- Memory-efficient batch processing for large files
- Simple command-line interface
- Optional quality settings for output files
- Dimension constraints for large images
- Configurable memory budget

## Requirements

- C++ compiler supporting C++17 or later (Xcode's clang)
- libheif (for HEIF/HEIC format support)
- libjpeg (for JPEG format support)
- Xcode Command Line Tools

## Installation

```bash
# Clone the repository
git clone https://github.com/yourusername/heif2jpeg.git
cd heif2jpeg

# Make sure Xcode Command Line Tools are installed
xcode-select --install

# Build the project using make
make
```

## Usage

### Basic Usage

```bash
./heif2jpeg /path/to/input/file.heic
```

### Convert Multiple Files

```bash
./heif2jpeg /path/to/input/file1.heic /path/to/input/file2.heic
```

### Convert All HEIF Files in a Directory

```bash
./heif2jpeg /path/to/input/directory/*.heic
```

### Specify Output Directory

```bash
./heif2jpeg -o /path/to/output/directory /path/to/input/file.heic
```

### Set JPEG Quality

```bash
./heif2jpeg -q 85 /path/to/input/file.heic
```

### Force Overwrite of Existing Files

```bash
./heif2jpeg -f /path/to/input/file.heic
```

### Set Maximum Image Dimensions

```bash
./heif2jpeg -w 4000 -ht 3000 /path/to/input/file.heic
```

### Set Memory Budget (in MB)

```bash
./heif2jpeg -m 1024 /path/to/input/file.heic
```

## Options

- `-q, --quality N`: Set JPEG quality (1-100, default: 95)
- `-f, --force`: Overwrite existing output files
- `-o, --outdir PATH`: Set output directory for converted images
- `-w, --maxwidth N`: Set maximum allowed image width (0 = unlimited)
- `-ht, --maxheight N`: Set maximum allowed image height (0 = unlimited)
- `-m, --memory MB`: Set memory budget in MB (0 = auto)
- `-h, --help`: Show help message

## Performance

The tool automatically:
- Detects the optimal number of threads based on your CPU
- Prioritizes processing smaller images first
- Manages memory usage to avoid system slowdowns
- Preserves all available metadata from the original files

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Acknowledgements

- [libheif](https://github.com/strukturag/libheif) for HEIF/HEIC support
- [libjpeg](http://libjpeg.sourceforge.net/) for JPEG processing

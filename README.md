# dia-merge

A tool to combine multiple .dia (llvm diagnostics file) files into a single .dia file.


## Supported features

* Merging several `.dia` files into a single file
* Remaps included sourcecode paths using regex
* Reading `.dia` file from a FIFO file

## Overview

`.dia` files are generated by swift and clang compilers to let the IDE know about warnings and fix-its. By default, each compilation file generates a single .dia file with an absolute path. When a build system needs (e.g. Bazel) needs to merge all individual .dia files into an aggregation, this tool combines them into a "fat" dia.

### Remapping

This tool supports also remapping. That might be useful in builds distribution where machines do not share the same absolute paths `dia-merge` supports `-remap` flag(s) which `<replacement_fix>=<substitute>`. You can provide several `-remap` arguments.

### Reading from a FIFO file

If you have a stream file to which compilers write their diagnostics, you can make `dia-merge` a consumer of that file. That may be useful if you don't want to pass unique paths to each compiler invocation and instead pass always the same path. Because a `dia-merge` is a reader of the fifo, it has to be running in the background a compilation happens, otherwise compiler will hang until some process reads written bytes. When `dia-merge` is interrupted with a signal `SIGINT`, it dumps all read diagnostics to the final output file.

### Example flow

1. Start dia-merge in the background with `dia-merge -s /path/fifo.file -o /path/merged.dia &`
2. Run a build and pass these parameters
* clang: `--serialize-diagnostics /path/fifo.file`
* Swift: `-Xfrontend -serialize-diagnostics-path -Xfrontend /path/fifo.file`
3. Interrupt the `dia-merge` with: `kill -2 %1`

As a result, `/path/merged.dia` will contain all diagnostics published to the stream `path/fifo.file`.

_Note: There is a limit of the `.dia` file that this mode guarantees consistency: 65536 bytes (limited by a macOS pipe buffer). Usually `.dia` are very small files (hundreds of bytes) so 65KB should be big enough. If a file will be bigger, it is possible that a kernel will not write dia bytes to a FIFO file atomically and if more than one compilation process writes to a file, a reader will my parse truncated `.dia` file content._

### DIA file

DIA file is a bitcode formatted file that can be inspected with LLVM's bc-analyzer tool.  

## Example

If a project has source root `/source_root/`, on a producer side call:


```
dia-merge -r `^/source_root/=./` -output /SomeDerivedData/Example/Build/Intermediates.noindex/Example.build/Debug-iphonesimulator/Example.build/Objects-normal/arm64/Example.dia /SomeDerivedData/Example/Build/Intermediates.noindex/Example.build/Debug-iphonesimulator/Example.build/Objects-normal/arm64/AppDelegate.dia /SomeDerivedData/Example/Build/Intermediates.noindex/Example.build/Debug-iphonesimulator/Example.build/Objects-normal/arm64/ViewController.dia 
```

On a consumer side, where a source root is `/other_source_root/`, call:


```
dia-merge -r `^\./=/other_source_root/` -output build_system/ExpectedLocation.dia  downloaded_package/Example.dia
```

## Build Instructions

To build, first download LLVM prebuilt binary from https://github.com/llvm/llvm-project/releases, e.g. https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.0/clang+llvm-13.0.0-x86_64-apple-darwin.tar.xz
So far, LLVM publishes only x86_64 architecture, so on arm64 machines, rosetta2 emulation is needed.

```sh
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_DIR=#{path_to_downloaded_clang} ..
ninja
```

See [RELEASING.md](RELEASING.md) for a fat-architecture.

Or, if you prefer Xcode for building and debugging, you can replace the last 2 lines with the following:

```
cmake -G Xcode -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_DIR=#{path_to_downloaded_clang} ..
open dia-merge.xcodeproj
```

#### Tip: full script to build an Xcode project

_Note: this snippet uses clang 13.0.0._

```
mkdir build
cd build
curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.0/clang+llvm-13.0.0-x86_64-apple-darwin.tar.xz" --output clang.zip
mkdir -p clang-downloaded
tar -xvf ./clang.zip -C clang-downloaded --strip 1
cmake -G Xcode -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_DIR="$PWD/clang-downloaded" ..
open dia-merge.xcodeproj
```
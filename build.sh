#/!bin/bash


: ${CLANG_VERSION:=13.0.0}

LLVM_PACKAGE_PATH="$PWD/llvm.tar.gz"
LLVM_UNZIPPED_PACKAGE_PATH="$PWD/llvm"


##### Download LLVM package


# -L to follow redirects
curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-${CLANG_VERSION}/clang+llvm-${CLANG_VERSION}-x86_64-apple-darwin.tar.xz" --output "$LLVM_PACKAGE_PATH"

mkdir -p "$LLVM_UNZIPPED_PACKAGE_PATH"
# --strip 1 to not include a wrapped dir and extract directly to $LLVM_UNZIPPED_PACKAGE_PATH
tar -xvf "$LLVM_PACKAGE_PATH" -C "$LLVM_UNZIPPED_PACKAGE_PATH" --strip 1

### Generate a project
# TODO: Using Xcode for now, as CI doesn't have ninja
mkdir -p build
pushd build
cmake -G Xcode -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_DIR="$LLVM_UNZIPPED_PACKAGE_PATH" ..

### Build a project
xcodebuild -configuration Release
popd

### Test the file
./build/Release/dia-merge --help

# wavebox
An Xbox controller adapter for the Nintendo Gamecube, inspired by the WaveBird controller.

## Manual Dependency Install
```bash
cd deps/
git submodule add https://github.com/ricardoquesada/bluepad32.git
git submodule update --init --recursive
cd bluepad32/external/btstack
git apply ../patches/*.patch
cd ../../../../ # return to base dir
```

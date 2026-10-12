# Audio spectral FFT dependency

The manifest baseline `256acc64012b23a13041d8705805e1f23b43a024` selects KissFFT **131.2.0, port revision 0**. The manifest requests that version. No tools or OpenMP features are enabled. The core port supports the repository's Windows triplets; tools and OpenMP have separate platform restrictions.

## ABI and project integration

The upstream baseline port builds float, double, int16_t and int32_t variants with identical C function names. Jamma selects a dependency-local overlay through `vcpkg-configuration.json`, using `overlay-ports: ["./vcpkg-ports"]`. Relative paths resolve from that configuration file; overlay ports take precedence over the registry. The remaining dependencies still use the manifest's builtin baseline. See the official [configuration reference](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-configuration-json) and [overlay resolution rules](https://learn.microsoft.com/en-us/vcpkg/concepts/overlay-ports).

`vcpkg-ports/kissfft` copies the baseline recipe, patches, usage and port manifest. The build change is solely `datatypes float`, preventing double/integer libraries from being installed into either lib or debug/lib by this port. This makes existing MSBuild wildcard auto-linking select the correct ABI without project-wide filtering or disabling auto-link. The wrapper includes `<kissfft/kiss_fftr.h>` and also asserts that `kiss_fft_scalar` is float. The source version, SHA512, patches, linkage and supported feature settings are unchanged.

The Windows float output is `kissfft-float.lib` and, for dynamic linkage, `kissfft-float.dll`. Existing vcpkg link/runtime integration remains authoritative. A previously installed multi-variant builtin package must be replaced by a normal manifest installation before relying on the overlay; no installed tree was modified during this implementation.

Register `src\audio\AudioSpectralFft.cpp` and `src\audio\AudioSpectralFft.h` in JammaLib's project and audio filters. JammaLib is a static library: the final application/test executable is responsible for resolving its KissFFT symbols. No header include-directory change is needed because the project already includes the vcpkg include root.

## Transform contract

`AudioSpectralFft` owns separate forward/inverse plans and reusable real/native-complex scratch through RAII. Construction, transforms and destruction belong to the dedicated worker. One instance must not be used concurrently. Failed plan/scratch allocation leaves `IsValid()` false.

The wrapper accepts power-of-two N >= 4, within the backend's signed size arithmetic. This avoids generic-butterfly temporary allocations; N = 2 would invoke the backend's one-point generic butterfly. Plans contain their internal temporary data, and transform buffers do not alias. The default N = 4096 is supported.

Forward returns N/2+1 unscaled complex bins; inverse applies 1/N once. Window and overlap-add normalization remain the STFT layer's responsibility. DC/Nyquist imaginary values are zero on forward and must be zero on inverse. Application bins are copied to native coefficients without type-punning. Nonfinite inputs and mismatched spans are rejected; nonfinite transform results are rejected without modifying the caller's destination. Exceptions are worker-side errors, never callback control flow. No peak limiting or whole-recording normalization occurs here.

## License and verification sources

KissFFT is BSD-3-Clause, copyright Mark Borgerding. Preserve the upstream copyright, conditions and disclaimer with redistributed binaries. The overlay installs both upstream COPYING and the full LICENSES/BSD-3-Clause through `vcpkg_install_copyright`, so the installed copyright includes the complete notice and license terms. The copied vcpkg recipe is MIT-licensed, copyright Microsoft Corporation; its complete notice is retained in `vcpkg-ports/kissfft/LICENSE.txt`. This integration does not vendor upstream FFT implementation source.

- [Versioned real-transform API](https://github.com/mborgerding/kissfft/blob/131.2.0/kiss_fftr.h)
- [Versioned implementation and endpoint handling](https://github.com/mborgerding/kissfft/blob/131.2.0/kiss_fftr.c)
- [Float scaling and arithmetic](https://github.com/mborgerding/kissfft/blob/131.2.0/_kiss_fft_guts.h)
- [Butterfly allocation and plan lifetime](https://github.com/mborgerding/kissfft/blob/131.2.0/kiss_fft.c)
- [Library naming and build options](https://github.com/mborgerding/kissfft/blob/131.2.0/CMakeLists.txt)
- [Copyright notice](https://github.com/mborgerding/kissfft/blob/131.2.0/COPYING), [full BSD-3-Clause text](https://github.com/mborgerding/kissfft/blob/131.2.0/LICENSES/BSD-3-Clause)

The baseline port, version table and patch files were read from the local vcpkg checkout. No dependency installation, build or tests were run. Runtime linking, numerical tolerance and throughput remain unverified until the user requests validation.

# SoapySDR radiod front end

`soapy.c` is a C hardware module for ka9q-radio's dynamically loaded radiod
front-end interface. It exports:

- `soapy_setup()`
- `soapy_startup()`
- `soapy_shutdown()`
- `soapy_tune()`
- `soapy_gain()`
- `soapy_atten()`

Setup opens the device, selects the sample rate and creates the stream, but
does not activate it. With no explicit `frequency` setting it also leaves the
tuner alone and reports a current first-LO frequency of zero. Shutdown
deactivates and closes the stream while retaining the configured device;
startup recreates the stream when necessary. Closing it makes quiescence work
even with Soapy modules whose `deactivateStream()` does little or nothing.

The shim requests CS16 when available, then CF32, then CS8. The `format`
setting can override this choice. Generic SoapySDR has one overall gain but no
separate attenuation API, so the shim implements radiod's controls as:

```
Soapy overall gain = radiod gain - radiod attenuation
```

Manual gain or attenuation requests disable hardware AGC. Absolute input-level
calibration is unknown unless `gaincal` is explicitly configured.

Apply the small additions in `Makefile.fragment` to `src/Makefile`, and add
`soapy.c` to its `CFILES` definition. Build with:

```sh
make ENABLE_SOAPY=1
```

The SoapySDR development headers, library, the selected hardware module, and
any vendor runtime library must be installed.

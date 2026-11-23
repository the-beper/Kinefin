# Cast root certificates

`cast_root_ca.der` ("Cast Root CA", valid until 2034-03-28) and `eureka_root_ca.der` ("Eureka Root CA", valid until 2032-12-12) are the roots a Google Cast device's certificate chain must end at. They are copied unchanged from the DER arrays in Chromium's Open Screen (`cast/common/certificate/cast_root_ca_cert_der-inc.h` and `eureka_root_ca_der-inc.h`), BSD-3-Clause, "The Chromium Authors". The SHA-256 fingerprints are `809af147…` and `caf6d1e3…`, the same as Chromium's own trust store. `src/cast/CastDeviceAuth.cpp` pins exactly these two.

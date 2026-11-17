#pragma once
// Firmware update over HTTP, on top of the Arduino Update library (same calls on both chips).
// One update at a time. Nothing here checks who is asking: HttpApi only routes an authenticated request here.
#include <stddef.h>
#include <stdint.h>

namespace espwf {
namespace Ota {

// `expected` is an upper bound on the image size (the upload's length). `md5` (32 hex chars) is optional but
// should always be given: the image is rejected at the end if it does not match.
bool begin(size_t expected, const char* md5);
bool write(const uint8_t* data, size_t len);
bool end();      // true = the image was written, verified and will boot after a restart
void abort();
bool busy();
int errorCode();  // 0 = none; otherwise the Update library's code
const char* errorText();

}  // namespace Ota
}  // namespace espwf

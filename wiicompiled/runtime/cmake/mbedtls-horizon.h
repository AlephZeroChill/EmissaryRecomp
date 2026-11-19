// Mbed TLS settings for Horizon (appended to the default config). Its Unix paths build on
// libnx's BSD sockets and newlib's clocks, but there is no /dev/urandom or getrandom().
#define MBEDTLS_NO_PLATFORM_ENTROPY
// The millisecond clock comes from libnx's system tick (runtime/src/hle/net/network_ssl.cpp).
#define MBEDTLS_PLATFORM_MS_TIME_ALT

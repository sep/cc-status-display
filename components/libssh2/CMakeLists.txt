# libssh2 as a local ESP-IDF component, built on ESP-IDF's bundled mbedTLS.
#
# Why vendored instead of the registry package (skuodi/libssh2_esp):
# ESP-IDF v6.0 ships mbedTLS 4.0 (TF-PSA-Crypto), which removed the legacy
# <mbedtls/rsa.h>, <mbedtls/ecdsa.h>, <mbedtls/ctr_drbg.h> … headers that
# libssh2 1.11.1's backend includes unconditionally, and upstream master's
# mbedTLS 4 path is unfinished. The submodule therefore points at
# mike-rogers/libssh2 branch `mbedtls-4-psa` (upstream master + a PSA-based
# mbedTLS 4 backend). Move it back to upstream once that work is merged and
# released. To bump: `git -C components/libssh2/libssh2 checkout <sha>` and
# commit the pointer.
set(_ssh2 "${CMAKE_CURRENT_SOURCE_DIR}/libssh2")

# src/Makefile.inc CSOURCES minus the openssl / libgcrypt / wincng / os400
# backends (empty without their defines anyway). blowfish.c is #included by
# bcrypt_pbkdf.c, so it is deliberately not listed.
set(_ssh2_srcs
    agent.c bcrypt_pbkdf.c chacha.c channel.c cipher-chachapoly.c comp.c
    crypt.c global.c hostkey.c keepalive.c kex.c knownhost.c mac.c mbedtls.c
    misc.c packet.c pem.c poly1305.c publickey.c scp.c session.c sftp.c
    transport.c userauth.c userauth_kbd_packet.c version.c)
list(TRANSFORM _ssh2_srcs PREPEND "${_ssh2}/src/")

idf_component_register(
    SRCS ${_ssh2_srcs}
    INCLUDE_DIRS "${_ssh2}/include"
    PRIV_INCLUDE_DIRS "." "${_ssh2}/src"
    PRIV_REQUIRES mbedtls lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBSSH2_MBEDTLS HAVE_CONFIG_H)
if(CONFIG_CC_SSH_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBSSH2DEBUG)
endif()

# Third-party code: keep ESP-IDF's -Werror=all from failing the build on
# warnings we don't own (platform type-size format strings and the like).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format -Wno-format
    -Wno-error=maybe-uninitialized -Wno-error=uninitialized
    -Wno-error=narrowing -Wno-error=sign-compare
    -Wno-error=unused-variable -Wno-error=unused-function
    -Wno-error=unused-but-set-variable -Wno-error=implicit-fallthrough)

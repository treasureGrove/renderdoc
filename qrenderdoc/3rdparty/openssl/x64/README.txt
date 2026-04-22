OpenSSL DLLs for HTTPS (Pipeline Agent) on Windows
====================================================

If your Qt bin folder does not ship libssl/libcrypto (common with minimal 3rdparty trees),
copy BOTH DLLs from the SAME Qt installation you used to build qrenderdoc into this folder,
then rebuild. The vcxproj copies *.dll from here to the output directory.

Examples (x64, match your Qt's OpenSSL version):
  libssl-1_1-x64.dll + libcrypto-1_1-x64.dll
  libssl-3-x64.dll   + libcrypto-3-x64.dll

Alternatively set environment variable RENDERDOC_OPENSSL_PATH to a folder containing
those DLLs (checked before the exe directory at startup).

References: Qt docs "SSL" / OpenSSL; your Qt version's bin directory.

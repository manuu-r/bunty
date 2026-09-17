"""libfvad requires C11; change C only, preserving Arduino's C++ standard."""
Import("env")

env.Append(CFLAGS=["-std=gnu11"])

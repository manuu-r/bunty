idf_component_register(SRCS "../../.deps/animatedgif/src/AnimatedGIF.cpp"
  INCLUDE_DIRS "../../.deps/animatedgif/src" REQUIRES bunty_platform)
target_compile_options(${COMPONENT_LIB} PRIVATE -include Runtime.h -fstack-usage -Wno-unused-function)

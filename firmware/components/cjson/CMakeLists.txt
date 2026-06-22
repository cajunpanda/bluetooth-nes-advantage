# Vendored cJSON (Dave Gamble, MIT). ESP-IDF normally ships this as the `json` component, but it is
# absent from the PlatformIO framework-espidf package, so we bundle the upstream release here.
# Source: https://github.com/DaveGamble/cJSON (v1.7.18). MIT, same as the firmware (see ../../LICENSE).
idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")

# Template test module (SPEC §5.8): copy to components/mod_<ic>/ for a new IC under test.
idf_component_register(SRCS "mod_example.c"
                       INCLUDE_DIRS "include"
                       REQUIRES devlink
                       PRIV_REQUIRES esp_timer freertos log)

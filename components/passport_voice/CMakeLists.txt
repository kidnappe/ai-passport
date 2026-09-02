idf_component_register(
    SRCS "src/passport_voice.c"
         "src/voice_ble.c"
         "src/adpcm.c"
         "src/voice_protocol.c"
         "src/audio_streamer.c"
         "src/voice_sound.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES json
    PRIV_REQUIRES bsp passport_core bt
)

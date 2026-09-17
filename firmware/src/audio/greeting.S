/*
 * Bunty's greeting, embedded as MP3.
 *
 * The Helix decoder is already linked for the voice gateway's spoken replies,
 * so shipping the compressed clip costs no extra code and saves ~215 KB of
 * flash over a pre-decoded PCM blob. Audio.cpp decodes it at boot.
 *
 * The path below is relative to the PlatformIO project dir (firmware).
 */
    .section .rodata.bunty_greeting, "a", @progbits
    .balign 4

    .global buntyGreetingMp3
buntyGreetingMp3:
    .incbin "src/audio/greeting.mp3"

    .balign 4
    .global buntyGreetingMp3End
buntyGreetingMp3End:

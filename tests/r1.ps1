# Round 1: does the replay reach the race, and which gating fixes the start billboard?
$configs = @(
    @{ name = "base" },                                                    # current default (billboard too fast)
    @{ name = "old";    env = @{ RR_MIRROR_OLD = "1" } },                  # scene setup always 60 Hz (billboard right)
    @{ name = "base2" },                                                   # repeat of base: is the replay deterministic?
    @{ name = "sprite"; env = @{ RR_SPRITE_GATE = "1" } },
    @{ name = "combo";  env = @{ RR_SPRITE_GATE = "1"; RR_GATE_EXTRA = "088ceab4,08835c40,088d8738,088d7ff0" } }
)

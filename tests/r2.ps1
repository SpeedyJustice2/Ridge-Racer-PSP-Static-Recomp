# Round 2: same configs with frames saved (GO+0 .. GO+240) so the billboard can be measured.
$configs = @(
    @{ name = "base" },
    @{ name = "old";    env = @{ RR_MIRROR_OLD = "1" } },
    @{ name = "sprite"; env = @{ RR_SPRITE_GATE = "1" } },
    @{ name = "combo";  env = @{ RR_SPRITE_GATE = "1"; RR_GATE_EXTRA = "088ceab4,08835c40,088d8738,088d7ff0" } }
)

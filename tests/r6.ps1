# Round 6: region fingerprint of the digital billboard (centre, straight ahead), GO+0 .. GO+700.
$common = @{ RR_REGION = "215,95,70,50;4705,700"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "700" }
$configs = @(
    @{ name = "base" },
    @{ name = "old";    env = @{ RR_MIRROR_OLD = "1" } },
    @{ name = "sprite"; env = @{ RR_SPRITE_GATE = "1" } }
)

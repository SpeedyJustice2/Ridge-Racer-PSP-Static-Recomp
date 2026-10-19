# Round 5: hires frames during the countdown (absolute frames 4400-4704; GO is at 4705 in the golden replay).
$common = @{ RR_SHOT_HIRES = "1"; RR_SHOT_RANGE = "4400,305"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "5" }
$configs = @(
    @{ name = "base";   args = @("--internal-scale","2") },
    @{ name = "old";    env = @{ RR_MIRROR_OLD = "1" }; args = @("--internal-scale","2") },
    @{ name = "sprite"; env = @{ RR_SPRITE_GATE = "1" }; args = @("--internal-scale","2") }
)

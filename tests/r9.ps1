# Round 9: 60 fps reference, hires frame every 1/4 s of game time for 10 s after GO (GO now detected at 60 fps too).
$configs = @(
    @{ name = "f60"; fps = @(); env = @{ RR_SHOT_HIRES = "1"; RR_GO_SHOTS = "0,600,15"; RR_GO_STOP = "600" }; args = @("--internal-scale","2") }
)

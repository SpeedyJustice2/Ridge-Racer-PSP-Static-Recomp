# Round 8: billboard pictures over game time. Hires frame every 1/4 s of game time for 10 s after GO.
$configs = @(
    @{ name = "f60";  fps = @(); env = @{ RR_SHOT_HIRES = "1"; RR_GO_SHOTS = "0,600,15";  RR_GO_STOP = "600" };  args = @("--internal-scale","2") },
    @{ name = "f120"; env = @{ RR_SHOT_HIRES = "1"; RR_GO_SHOTS = "0,1200,30"; RR_GO_STOP = "1200" }; args = @("--internal-scale","2") }
)

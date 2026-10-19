# Round 13: billboard draw parameters (uv, colour, blend) per draw, 60 vs 120, ~4 s after GO.
$configs = @(
    @{ name = "f60";  fps = @(); env = @{ RR_TEXUV = "04160000,04166000;4700,240"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "250" } },
    @{ name = "f120"; env = @{ RR_TEXUV = "04160000,04166000;4705,480"; RR_GO_SHOTS = "-1,0"; RR_GO_STOP = "490" } }
)

# r15: audio recording. Same recorded input, real 60 fps game twice (is the audio the same every time?) and the port at 120 fps twice.
$common = @{ RR_AUDIOLOG = "1"; RR_GO_SHOTS = "0,1" }
$configs = @(
    @{ name = "ref60a";  fps = @(); env = @{ RR_GO_STOP = "1800" } },
    @{ name = "ref60b";  fps = @(); env = @{ RR_GO_STOP = "1800" } },
    @{ name = "port120a"; env = @{ RR_GO_STOP = "3600" } },
    @{ name = "port120b"; env = @{ RR_GO_STOP = "3600" } }
)

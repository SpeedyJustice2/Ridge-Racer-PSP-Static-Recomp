# r18: frame-rate sweep after the cam_started fix (a10), internal scale 3.
$common = @{ RR_GO_SHOTS = "0,1" }
$configs = @(
    @{ name = "fps90";  fps = @("--fps","90","--fps-patch","car","--no-low-latency","--internal-scale","3");  env = @{ RR_GO_STOP = "2700" } },
    @{ name = "fps144"; fps = @("--fps","144","--fps-patch","car","--no-low-latency","--internal-scale","3"); env = @{ RR_GO_STOP = "4320" } },
    @{ name = "fps180"; fps = @("--fps","180","--fps-patch","car","--no-low-latency","--internal-scale","3"); env = @{ RR_GO_STOP = "5400" } },
    @{ name = "fps240"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3"); env = @{ RR_GO_STOP = "7200" } }
)

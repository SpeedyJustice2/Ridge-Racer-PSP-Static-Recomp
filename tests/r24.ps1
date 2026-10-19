# r24: what happens when the host can't keep the rate? Game time vs wall time (HEARTBEAT GAMETIME) with a forced GPU overload (internal scale 16).
$common = @{ }
$configs = @(
    @{ name = "fps240_s3";  fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3") },
    @{ name = "fps240_s16"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","16") },
    @{ name = "fps120_s16"; fps = @("--fps","120","--fps-patch","car","--no-low-latency","--internal-scale","16") },
    @{ name = "fps60_s16";  fps = @("--internal-scale","16") }
)

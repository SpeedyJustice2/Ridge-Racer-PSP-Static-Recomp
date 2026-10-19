# r22: 240 fps at internal scale 3 / 5 / 8 after the vertex-path speedup (no profiler, GPU backlog log on).
$common = @{ }
$configs = @(
    @{ name = "fps240_s3"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3","--latency-log") },
    @{ name = "fps240_s5"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","5","--latency-log") },
    @{ name = "fps240_s8"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","8","--latency-log") }
)

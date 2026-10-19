# r19: performance. RR_PROF=1 (time per GE/GL section per 300 frames), --latency-log (GPU backlog), internal scale 3 vs 8, 240 and 144 fps.
$common = @{ RR_PROF = "1" }
$configs = @(
    @{ name = "fps240_s3"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3","--latency-log") },
    @{ name = "fps240_s8"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","8","--latency-log") },
    @{ name = "fps144_s3"; fps = @("--fps","144","--fps-patch","car","--no-low-latency","--internal-scale","3","--latency-log") },
    @{ name = "fps240_s3_nomsaa"; fps = @("--fps","240","--fps-patch","car","--no-low-latency","--internal-scale","3","--msaa","1","--latency-log") }
)

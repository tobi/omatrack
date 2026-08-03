use cosworth_telemetry::CosworthFile;
use motorsport_telemetry_core::TelemetrySource;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args()
        .nth(1)
        .expect("usage: dump_structure FILE.pds");
    let file = CosworthFile::open(&path)?;
    println!("path={} channels={}", file.path(), file.channels().len());
    let mut max_name = 0usize;
    let mut max_unit = 0usize;
    for (i, ch) in file.channels().iter().enumerate() {
        max_name = max_name.max(ch.name.len());
        max_unit = max_unit.max(ch.unit.len());
        let periods: Vec<u64> = ch.chunks.iter().map(|c| c.sample_period_ns).collect();
        let uniq_period = periods.iter().collect::<std::collections::BTreeSet<_>>();
        // verify chunk time contiguity
        let mut contiguous = true;
        let mut expect = 0u64;
        for c in &ch.chunks {
            if c.time_base_ns != expect {
                contiguous = false;
            }
            expect = c.time_base_ns + c.sample_count * c.sample_period_ns;
        }
        println!(
            "[{i:2}] id={:<6} type={:<8} chunks={:<4} samples={:<8} dur_ns={:<14} periods={:?} contiguous={} name={:?} unit={:?}",
            ch.id,
            ch.sample_type.name(),
            ch.chunks.len(),
            ch.sample_count,
            ch.duration_ns,
            uniq_period,
            contiguous,
            ch.name,
            ch.unit
        );
    }
    println!("max_name_len={max_name} max_unit_len={max_unit}");

    // value range / precision probe: does f32 suffice?
    println!("\n--- f32 sufficiency probe (all samples) ---");
    for (ci, ch) in file.channels().iter().enumerate() {
        let mut lossy = 0u64;
        let mut total = 0u64;
        let mut worst = 0.0f64;
        for (chunk_i, chunk) in ch.chunks.iter().enumerate() {
            for s in 0..chunk.sample_count {
                let v = file.decode(ci, chunk_i, s);
                total += 1;
                let rt = v as f32 as f64;
                if rt != v {
                    lossy += 1;
                    let err = (rt - v).abs();
                    if err > worst {
                        worst = err;
                    }
                }
            }
        }
        if lossy > 0 {
            println!(
                "{:<28} f32-lossy {}/{} worst_abs_err={:.6e}",
                ch.name, lossy, total, worst
            );
        }
    }
    Ok(())
}

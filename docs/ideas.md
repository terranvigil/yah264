# Ideas backlog

A running list of optimizations, coding tools, and differentiating features worth
building. Kept alongside the main-feature work so ideas that surface mid-implementation
don't get lost. Not a plan of record; items graduate to `plan.md` when scheduled.

Status tags: `idea` (unvetted), `planned`, `wip`, `done`, `dropped`. Benefit and
effort are rough (S/M/L). "BD-rate" means quality-per-bit versus x264 unless noted.

## Implemented but sub-optimal (revisit when feature-complete)

Features that work and are shipped, but were built bounded / simplified / with
placeholder constants to keep scope contained. This is the "come back and tighten
this" list -- kept current as features land so the shortcuts are not forgotten.
Quality shortcuts first (BD-rate on the table), then speed, then tunables.

| Feature | Current (shipped) | Optimization once feature-complete |
|---|---|---|
| Reference-frame staircase v1 (Y264_STAIR off) | anchor+B overlap via per-row consumability + fixed list-1 clamp (LAG 4); deterministic at every thread count, BD ~0.00%; but nested full-width pools OVERSUBSCRIBE, so it wins only with spare cores (t8 1.09x single-GOP 720p on 18 cores; t18 0.97x) | Lever-3 v2: a SHARED-pool multi-frame wavefront (one worker set pulls rows from several frames' grids, the shape x264 uses); would subsume FPIPE's split pools too |
| Content-adaptive ME, frame-level | KILLED at the measured ceiling: per-frame lowres-MV motion score gates UMH-off + capped subpel (f->me_cheap -> TLS in me.c; env Y264_ADME=thresh, default OFF = byte-identical). Signal is RIGHT on catastrophe (bus min-score 444 -> 0% cheap, BD untouched) but the 'cheap wins on static' premise was CUSHION: uniform-cheap costs +2..6% NEG on every clip (foreman +2.85, mobile +5.11, stefan +15.14, bus +40.54, coastguard -6.81, tempete -2.33, mean +9.1 -- coastguard/tempete only stay negative because they start 11/4.6 ahead). mobile is the decisive counterexample: LOWEST motion score (median 20) yet +2.7 damage -- dense-texture slow pans need subpel/UMH precision, so motion magnitude is anti-correlated with cheap-safety and no threshold separates damage from savings. Best frontier T=40: corpus mean -0.34% but mobile +4.38 (> its +3.97 limit) at only 10.0% corpus speed (mobile alone is 1.50 s of the 2.34 s saved; excluding it -> 3.6%). Kill bar was >=15% at mean<=0 -> no such configuration exists | revisit only with a cheapness signal that is not motion magnitude (e.g. subpel-gain feedback or texture-conditioned), or a milder cheap tier; per-BLOCK lowres-oracle gating already failed separately (weak signal, docs/adaptive-me-design.md) |
| RDOQ round-to-nearest seed | trellis/greedy seed at bias 32/64 + skip probe quantizes with the coder's seed (corpus -2.83% NEG, park_joy -6.66%); costs ~5-12% single-thread speed, mostly fewer early-skip commits at high QP (admitted MBs run full analysis then usually still pick skip via RD) | MITIGATED: the RD-middle-path idea was built and REFUTED (66% of admitted MBs commit but the trial is itself a full RD -> only ~2% back, and BD breaks -1.6 VMAF/+2% bits on samsung -- searched-MV winners get mis-skipped); the SHIPPED fix is the trellis-ALIGNED probe (probe_signif_rdoq): when the seed keeps a coefficient, ask the coder's own Viterbi trellis whether any level survives -- all-zero commits skip. Decomposition (samsung 100f 1t pure-C): seed-fix cost 9.6%, admissions 8.4pt of it, trellis density only 1.2pt; the aligned probe recovers 5.1% wall (samsung 300f, 2.4% foreman CIF) with a better BD (6-clip NEG mean -1.21% vs -0.99%, bus +10.27 within noise), conformance 249/249, Y264_PROBE_TRELLIS=0 escape byte-identical. Residual ~3pt = quality-carrying admissions + probe compute (irreducible without giving back the BD win); DC-block SEED refuted: round-to-nearest DC (Y264_DC_SEED64=32) is WORSE (bus +3.0/mobile +4.1/foreman +3.8 NEG) -- DC is low-freq, keeping more DC coeffs costs bits with no texture benefit; DC correctly wants the deadzone. A full DC TRELLIS is a different, unexplored lever but the seed signal suggests low headroom. Residual +3-5% points at mode-decision SSD (BUT psy-rd 2.0 vs 1.0 post-seed-fix = bus +0.49/coastguard +0.55/mobile -0.60 NEG -- the mode-decision TEXTURE/psy axis is TAPPED after the seed fix, bus gets worse; so bus's +9.79 residual is NOT coefficient- or mode-decision-texture, it is the zoom-motion coding floor -- a narrow, hard, deep-effort residual, not a quick lever) |
| mb-tree | windowed: mini-GOP B's + anchor-chain over the rc-lookahead window, B configs only; offsets are non-centred under CRF/ABR (measured -3.6% BD-rate foreman CRF, bframes 3) and zero-mean at CQP | IPPP chain loses at BOTH offset forms (+2.4% CQP centred, +3.6% CRF non-centred: the single long chain saturates propagation and the log2 term goes flat-large); needs an fps-scaled decay and qcomp coupling. Propagate through B links (both lists). 2-pass excluded from non-centred (stats pass is fixed-QP). inv_qscale weighting on the B-propagation path alone tried and reverted: anchor-lowres-variance -> 2^(-aq_off/6), normalised mean 1, weighting the splat amount -- CRF-VMAF foreman -0.07% (flat) but mobile +0.75% / akiyo +1.14% (net loss). The partial form loses; x264 applies inv_qscale with an fps factor, on BOTH lists including the chain, with a qcomp-coupled strength AND folds qp_offset_aq into a single combined offset (we sum aq_off + mbtree_off) -- the pieces have to land together, not one at a time. The full form was also tried and reverted: inv_qscale in BOTH the chain and B-loop (from lowres 8x8 variance, absolute) + strength 2.0 (=5*(1-qcomp)), landed atomically -- CRF-VMAF LOSS at every strength (foreman +2.4..+3.9%, akiyo +2.3..+3.5%, mobile flat, strengths 1.5/2.0/2.5). Root cause identified: our AQ (aq_analyze) uses full-res 16x16-MB variance applied at ENCODE time, but the lookahead-side inv_qscale used lowres 8x8 variance -- so mb-tree couples to a DIFFERENT measure than the AQ actually applied and the two fight. Real prerequisite (architectural, not a formula tweak): compute the actual full-res AQ offsets in the LOOKAHEAD (where x264 runs AQ analysis), feed those exact qscales as mb-tree's inv_qscale, and fold qp_offset_aq into a single combined offset instead of summing aq_off + mbtree_off. Until AQ analysis moves push-side, the inv_qscale coupling can't match. A FOLLOW-UP disproved that hypothesis: recomputed inv_qscale from the exact full-res 16x16 variance aq_analyze uses (per source frame: en->plane[0] for the chain, bplane[b][0] for the B-loop) -- STILL a CRF-VMAF loss, akiyo worst (+7.8/+4.6/+1.8% at strength 1.5/2.0/2.5, foreman +2.1..+3.8%). So the variance source was NOT the blocker. Real blocker: we SUM aq_off + mbtree_off, and inv_qscale pushes MORE propagation onto low-variance blocks -- exactly the blocks AQ already lowers QP on -- so flat regions get doubly favoured (akiyo's static background over-spent vs the face). x264 avoids this by folding qp_offset_aq into ONE combined mb-tree offset. Conclusion after 3 attempts: inv_qscale is counterproductive UNLESS the combined-offset restructure lands with it (compute_mbtree emits qp_offset_aq - strength*log2(...) as the single offset and the separate aq_off application is disabled where mb-tree runs). That restructure -- not the variance source, not the formula pieces -- is the actual gate to the mb-tree lever, and even then the win is unproven for our formulation. COMBINED-OFFSET restructure built + measured (patch at scratchpad/mbtree_combined.patch, reverted): a single per-MB offset = qp_offset_aq - strength*(log2(intra*invq + prop) - log2(intra*invq)), strength 2.0, inv_qscale in the finish denominator (the anti-double-count) + propagation, separate aq_off disabled where mb-tree runs. The result flips the story: it WINS VMAF-NEG on every clip (akiyo -3.7%, foreman -0.5% v1 & NEG) and WINS CQP PSNR (foreman -1.0%), but LOSES VMAF-v1 on static akiyo (+5%) at every strength 0.75-2.5. Root cause is NOT a bug: the correct anti-double-count reallocates bits off akiyo's large flat background onto the moving face; VMAF-NEG (enhancement-discounting, the honest metric) and motion clips reward this, but VMAF-v1 and PSNR are dominated by akiyo's easy background area and penalise it. The NEG win and the v1 loss are the SAME mechanism -- cannot be decoupled. That made it an owner decision, not a formula tweak: either (a) adopt VMAF-NEG as the primary CRF gate for mb-tree (it wins cleanly everywhere), or (b) content-adaptive mb-tree strength from the shot class. **SHIPPED: (a), VMAF-NEG as the primary CRF gate.** The combined offset is the default at strength 2.0. A robust 5-point/120f CRF sweep confirms VMAF-NEG wins ALL clips: foreman -0.49%, mobile -0.15%, akiyo -1.82%. VMAF-v1 still regresses akiyo (+2.9%) by design -- that is flat-background spend NEG correctly discounts. Remaining mb-tree refinements: IPPP chain (still mbtree_on=bframes>0), both-list bipred propagation. IPPP with the combined offset STILL blows up: bframes-0 CRF vs mb-tree-off measured mobile +64.9% VMAF-v1 / +18.7% NEG (catastrophic), foreman +3%, only akiyo wins (-12/-15%). The combined offset fixed the AQ double-count but NOT chain saturation -- with every frame an anchor and no B split, the single long dependency chain over-propagates on busy content. IPPP needs fps-scaled propagation decay along the chain (a separate mechanism from the AQ fold), so it stays gated to bframes>0 |
| Lowres analysis | window analyses each frame vs prev + anchors vs prev anchor; core re-analyses at pop (duplicate work). The anchor pair + per-B pair fields run behaviour-matched lowres ME (reverse-scan median predictors, hex+square, subpel, colocated motion propagation) when a seed consumer is active (Y264_NO_UMH / Y264_RICH_SEEDS / Y264_B_SEEDS; default UMH keeps the legacy from-zero diamond = byte-identical + no speed cost). That brings the hex-only bus gap from +16.5 to +10.9 (= UMH level); UMH+all-seeds vs main measured bus -1.56 / park_joy_720p -3.10 NEG with stefan +0.71 -- a candidate default flip pending a full-corpus sweep | share push-side analysis with the core; add backward costs for b-adapt (the B pair NEXT legs EXIST -- the b-adapt over-demote note, sintel hard-cut +5%, can re-test with seeded pair costs); feed VBV; consider flipping rich_seeds+b_seeds on under UMH (needs stefan-safe full-corpus BD) |
| Motion estimation | UMH (uneven cross full-x/half-y + multi-hex grid, range 16) + spatial-neighbour MV seeds on the P 16x16 search (foreman -0.40% BD-PSNR, ~free; the same seeds on the B L0 16x16 search were tried and reverted: +0.12/+0.10% foreman/mobile -- L0 spatial neighbours are poor predictors for bidirectional content and interact badly with the Bi-reuse-of-L0 path) + SATD subpel, multi-ref with per-partition (mixed) refs; runs the full window scan on every partition incl. 4x4 sub-parts (~29% slower foreman) | >=8x8 partition gating measured + deferred: recovers only ~5.5% (sub-parts are rare) and costs +0.05/+0.10/+0.02% BD-PSNR foreman/mobile/akiyo -- a quality regression, not worth it while quality is the main goal; revisit as a motion-adaptive early-out once speed is prioritized. ESA/TESA; weighted-ref ME (search the WP-scaled plane on fades) |
| Multiple references | B list0 bounded to past refs (POC-descending default order); list1 single-ref; tail-flush P pins one ref; Bi reuses the winning L0 ref | allow future refs in the B list0 tail; multi-ref list1; joint L0xL1 Bi refinement |
| Inter mode decision | MB-level decision is already full RD (skip/16x16/16x8/8x16/P8x8/intra all compared on J = SSD + lambda*bits in the P-slice loop); only the MV/ref/sub-shape *selection within* a partition uses ME/SATD cost -- which matches x264 medium (RD ref/shape selection is a slower-preset feature). Not a medium-parity gap (verified in code) | RD ref + sub-shape selection (x264 slow/placebo, marginal at medium): RD-encode each ref x each shape instead of taking the ME-cost winner -- expensive for a small gain. Tried + reverted: RD ref-select and qpel-RD both help static (akiyo -0.2..-0.4%) but regress motion (foreman +0.1..+0.3%), net neutral-to-negative. Root cause: a per-MB greedy RD optimum changes the reconstructed ref pixels AND the MV field later neighbours predict from; on motion that downstream coupling (worse MVP, larger later mvds) outweighs the tiny local gain (SATD already tracks coded cost to sub-0.4%). A real win needs the RD score to account for predictor/rate coupling to later MBs, beyond a greedy per-MB stage |
| P_8x8 | full sub_mb_types (8x8/8x4/4x8/4x4), per-8x8 ref; shape picked at the 8x8-winning ref only | joint shape x ref search; B_8x8 sub-partitions (sub_mb_type in B slices) |
| Insurance-RD admission gate (A1b) | subme<=8 path always full-RD's 16x16 as insurance when the SATD winner is another shape. x264's subme-7 admission gate (skip when 16x16 SATD > 5/4 * winner) is BUILT + measured but ships OFF (Y264_RD_ADMIT, default 0 = byte-identical to HEAD): ~2-3% faster on both scalar+NEON but only VMAF-NEG-neutral with per-clip scatter (foreman +0.50/tempete +0.55/akiyo -0.57/mobile -0.63, mean ~-0.1%) -- a small speed gain doesn't justify +0.5% motion regressions on a max-quality default. CORRECTION on A1's "3-5x": the fan-out IS real (~12 full-MB RD encodes/MB -- partition SELECTION is SATD, but each chosen shape runs qpel_rd_nudge = ~6 full-MB RD re-encodes, x2 for the winner + the 16x16 insurance). A1(b) only trimmed part of ONE nudge chain -> ~2-3%. The fan-out's COUNT is untapped, but its COST is dominated by the CABAC bit-estimate (est_inter_mb_bits), so cutting the count via luma-only trials (A1a) saves little AND ranks MVs worse (Q4 mode). So 3-5x is an A3+A1-TOGETHER result: A3 makes each of the ~12 estimates cheap (table-walk, skip the residual re-walk), THEN trimming the nudge trial count pays. A3 is the prerequisite lever, not A1 alone | A3 first (cheap per-candidate est), then reduce qpel_rd_nudge trial count + wire A1b into fast presets; A1(c) chroma-once likely marginal until A3 lands |
| Direct temporal | opt-in (--direct temporal); per-slice fallback to spatial when a co-located reference is not in list0; since 2026-09-03 the default is `--direct auto`: each B slice picks spatial or temporal by the running skippability score, sampled on one macroblock in four (`Y264_DIRECT_AUTO_STRIDE`), under the staircase (`Y264_STAIR_TDIR=1`); pin with `--direct spatial` or `temporal` | --direct auto (per-frame winner heuristic); use temporal on fades where spatial's zero-MV bias hurts |
| Psy-RD | MB-level texture-energy term in the mode-decision J only (skip/inter/intra/partition choice); the metric is averaged 4x4-SATD-AC + 8x8-SA8D-AC energy difference (8x8 Hadamard >>2 to the SATD scale), default strength 1.0. Adding the 8x8-SA8D support beat the 4x4-only metric on BOTH VMAF-v1 and VMAF-NEG at every clip (v1 -1.5/-0.7/-1.0%, NEG -0.9/-0.3/-0.8% foreman/mobile/akiyo), ~4.6% slower -- NEG negative confirms genuine texture retention, not sharpening | subpel-psy (SATD-domain, likely needs the residual formulation the intra-loop psy did); per-strength recalibration (1.5/2.0 mix NEG on foreman). psy-trellis tried and reverted: a residual-AC-retention term in rdoq_4x4 (penalise J when the reconstructed 4x4 residual sheds Hadamard-AC energy vs the source residual) is CONTENT-DEPENDENT -- akiyo (static) wins -1.4..-3.4% VMAF-v1 but foreman/mobile (motion) LOSE +1..+3% at every strength (0.2/0.5/1.0), net negative on the corpus. Keeping AC energy over-spends where the residual is already large. The real form needs content-adaptive strength (off/low for high-motion, higher for static/detail) driven by the shot class -- that ties to the shot-based plan, not a flat global weight. Also tried: a coefficient-domain reward (subtract psy*lambda*sum|dequant AC| in the trellis), a residual-AC taper, and an intra-only gate -- none broke the tension. Retaining AC energy is exactly what VMAF-NEG penalises when the retained energy is prediction noise/aliasing, so motion clips (foreman, mobile) show NEG regressions at any strength that gives akiyo a v1 win; apparent mid-strength wins were 3-point BD-fit artifacts (sign flips 0.25 lose/0.3 win/0.35 lose). x264 ships psy-trellis OFF by default -- it's a niche knob, not a parity gap. The same AC term inside the intra-mode SATD loops measures a BD-VMAF LOSS: SATD-domain psy needs a residual-based formulation, not a pred-vs-src energy diff. FINAL: the gap-analysis domain fix implemented faithfully -- J -= psy*sum_AC|fdct(clip(pred+res))|, the true per-coefficient RECONSTRUCTION spectrum (not residual, not dequant-domain), in inter luma 4x4+8x8 RDOQ, env Y264_PSY_TRELLIS (default 0, off-state byte-identical). The domain fix DID kill the content tension on CLEAN content (no motion losses; 7 CIF clips within +-0.5% at 0.2/0.5). GRAIN EVAL (park_joy + ducks 720p): it WINS on grain and the win GROWS with strength -- park_joy 0.6/0.9/1.2 = -0.29/-0.49/-0.64%, ducks 0.9/1.2 = -0.35/-0.75% VMAF-NEG, not plateaued at 1.2. Content-dependent (the inverse of clean, where high strength loses), so no global strength; the ship path is shot-adaptive strength (~1.2 grain, off clean) per docs/shot-based-plan.md. Implementation done+correct, only the gating remains. That proves the corpus, not the code, was the blocker. **SHIPPED as `--tune grain` (strength 1.0) + the direct `--psy-trellis F` knob** (param.psy_trellis threaded to f->psy_trellis; Y264_PSY_TRELLIS still overrides for A/B). Off by default -> byte-identical; the manual/tune knob is the interim gate until shot-adaptive strength (docs/shot-based-plan.md) makes it automatic. `--tune grain` output is byte-identical to the measured Y264_PSY_TRELLIS=1.0 streams. **The class is FLAT/DARK content, not grain**: constant 1.2 at matched rate reads sintel -7.68 / akiyo -1.71 / tempete -1.17 / coastguard -1.01 / samsung -0.94 vs park_joy +0.05 and touchdown +2.26; the automated gate `Y264_PSY_FLAT_GATE=50,307,25` (per-frame flat-MB share) is quality-clean on all three instruments but default-REFUSED at samsung +30% t12 wall (armed frames take the greedy RDOQ path). The ship route for a default is a reconstruction-spectrum psy term inside the Viterbi lattice; the coefficient-domain trellis reward stays measured-refused |
| Weighted prediction | frame-level DC fade detection, one weight per ref | block/region-level weights; chroma weights |
| Variance AQ | x264 aq-mode 2 (auto-variance: per-MB log2(var) centred on the frame mean). aq-mode 3 dark-bias hook added (Y264_AQ_DARK, amplifies AQ on low-luma MBs where banding shows) but OFF by default: negligible on the current corpus (foreman -0.00/mobile +0.01/tempete +0.18/coastguard -0.04% VMAF-NEG -- mostly bright content), needs dark/banding clips + subjective eval to tune | joint calibration with mb-tree (the combined-offset fold); tune aq-mode 3 once a dark corpus exists |
| B-QP cascade | fixed per-depth offsets | derive per-frame from lookahead importance (mb-tree extension) |
| ABR rate control | single-pass, per-frame-type complexity->bits scales (I/P/B tracked separately; uncalibrated types fall back to any calibrated one then 1.0). Per-type scales beat a single blended scale by a large margin: BD-VMAF-v1 foreman -2.6 / mobile -12.7 / akiyo -5.2% (NEG similar) -- the blended scale badly mispredicted the far-costlier I frames, wrecking allocation | 2-pass-style forward complexity from the lookahead; VBV coupling |
| VBV | one-sided cap. Capped VBR composes across GOP joins: a GOP that follows another starts its buffer at half rather than full, and half is `vbv_fill_budget`'s own fixed point, so the segment exits where it started without any enforcement and concatenation is legal by induction with no cross-worker traffic. An explicit exit constraint was built and ABLATED AWAY -- it never bound on 30/36 cells and where it bound it front-loaded QP without cutting total spend, costing a cell and 600 kbit/s. Costs the multi-GOP clips ~7% of their bits and 1-3 VMAF-NEG points, which is what they were overspending. `scripts/cvbr_compliance.sh` 34/36 at both windows vs x264 18/18; samsung keyint 30 went from 9 underflows to 1. ABR+VBV still resets per GOP and composes only because its integrator holds the average (measured 36/36, a tendency and not an invariant) | the two failing cells are one mid-stream scene cut, not a join: the measured-size re-encode bound fixes it but only reaches `emit_frame`, and at 18 threads the stair/fpipe routes code 152 of 180 frames. The bits model still resets per GOP alongside the buffer (11% over cap at keyint 30 vs 7% at 250). HRD parameters in the SPS, so a third-party checker has something to verify against |
| CRF | absolute complexity vs a resolution-scaled base (x264-style), foreman-anchored, easy-content discount clamped at -2 QP (stands in for the mb-tree gains x264 spends there); ME-compensated lookahead cost as the metric measured slightly worse than full-res zero-motion SATD | uncapping the discount still LOSES post-mb-tree: caps -4/-8 vs -2 measured akiyo VMAF-NEG +4.55/+6.30% (foreman/mobile unaffected -- not easy enough to hit the discount). Even with the combined-offset mb-tree redistributing bits, easy content is VMAF-saturated near the operating range, so a lower QP is pure overspend. The -2 cap stands; mb-tree redistributes WITHIN a frame but can't make saturated easy content benefit from more total bits. Remaining CRF items: per-title base adaptation; CRF-mode BD vs x264 medium after this change: foreman +13.2 / mobile +19.3 / akiyo +9.0 (from +14.3 / unbounded overspend / +7.3). **The absolute-complexity branch is UNREACHABLE on the default path (crf_cl && mbtree_on), and it is also the wrong model: x264 has NO frame-level complexity term once mb-tree is on (the rate equation drops the blurred complexity; the instrumented reference's average rate-control QP is 31.03 on every CIF clip, 32.80 on 720p50, i.e. crf + 5.4 + 2.4*log2(0.04*fps) exactly). All of x264's CRF adaptation is the DC of the per-MB AQ field, which OUR AQ removes by centring on the frame mean (x264 anchors on an absolute 14.427). Y264_CRF_CPLX restores it + the fps term: BD-VMAF-NEG better on 9/12 clips (mobile -7.57, ducks -7.40, park_joy -5.50), worse on samsung +9.30 and touchdown +10.71, and size-at-equal-CRF vs x264 goes from a sign-flipping [-54.6,+45.3]% spread to [-29.7,+11.4]. DEFAULT OFF pending the chroma-energy fix (our AQ metric is luma-only, which is why the anchor needs a fitted 7.5 instead of the derived 6.427, and the two losers are flat-content clips). Do NOT re-derive CRF_BASE_CPLX/SLOPE/CAP -- that term does not exist in x264. sintel stays 25-42% under x264 at every CRF and is unexplained** |
| RDOQ | greedy with EXACT pixel distortion, iterated to convergence: elimination + bidirectional level nudging -- toward zero (fewer bits) AND away from zero (deadzone recovery toward round-to-nearest, the away probe runs only when the toward move is rejected). Bidirectional nudging measures foreman -0.36 / mobile -0.93 / akiyo -0.31% BD-PSNR, ~17% slower whole-encoder (44% single-thread RDOQ) -- it belongs behind a slower preset once the ladder lands. Key finding: for RDOQ that already computes exact idct distortion, a coeff-domain Viterbi is NOT the path (it trades exact for separable-approx distortion, the same trade that sank DC-trellis >1dB); a WIDER exact search beats greedy. cost is per-coder (CAVLC tables, or CABAC context-state estimate, cbf neighbour terms approx 0/0); no DC-block RDOQ | probe raising currently-zero coefficients (needs source-coef sign threaded in) + multi-step up-moves; exact cbf neighbour contexts; the CAVLC path still scratch-codes in the RD loop (top speed win). DC-block RDOQ tried and reverted: a lone-DC linear distortion model (idct of the DC error) lost >1 dB even at lambda 0 -- the idct's +32>>6 rounding makes +-1/pixel artifacts the same magnitude as the decisions being judged, so exact evaluation needs the AC levels and diffs threaded through the DC stage; DC-trellis gains are tiny elsewhere, so low priority. Transform-domain AC distortion for the FAST path tried and reverted: derived the exact separable model -- the 4x4 inverse columns are orthogonal with per-frequency norms [4,2.5,4,2.5], so pixel SSD = sum K[pos]*(level-ideal)^2 with K folding basis energy and dqstep^2; validated to RANK candidates identically to the idct (100% at qp>=22, 99.7% at qp16). But it is NOT a speed win: in double precision it is break-even-to-slightly-slower (foreman medium 3.80->3.91s) because the light RDOQ makes few probes/block and scan_bits is already O(16)/candidate, and an integer fixed-point form overflows int64 (Efp^2 * per-position weights). The real RDOQ speed lever is the CAVLC/CABAC scan_bits cost per candidate, not the distortion |
| Deblock filter | scalar `filter_line` | NEON (and x86) vectorization |
| DCT/IDCT butterflies | scalar C (4x4 and 8x8) | NEON kernels (quant/dequant already are) |
| dct-decimate | inter-only, per-block: drop a 4x4/8x8 residual block whose only nonzeros are isolated +-1 coeffs (decimate score < threshold). Thresholds 3/2, LIGHTER than x264's 6/4 because our full RDOQ already drops marginal coeffs (x264's threshold over-decimated: net PSNR loss; 3/2 nets -0.25% PSNR + speed). Y264_DCTDEC=0 disables | tune threshold per shot class; extend to intra (x264 doesn't) |
| P skip-exit dials | Y264_P_SKIP_EXIT (default 0): 1 = post-RD intra bypass when skip beats inter's RD; 2 = post-16x16-SATD commit (x264's B-exit shape at their P placement); 3 = intra admission bar tightened to min(inter, skip) SATD. exit2 -6.2% samsung-lo wall but band-refuted on every clip (+3..+27 BD); exit1 -2.2% wall, refused on sintel CRF +3.2 / touchdown ABR +15.5; mode 3 a measured null (bars differ only by priced bits, under the 1.5x margin). The P late-skip class (35% of P_Skip verdicts, ~390 ms on the samsung cell) is structurally protected: our late skips are cheaper-rate choices x264 never finds | none; reopen only with a changed question (cheaper trials, not fewer) |
| Reference-plane borders | 32/16-px edge-replicated borders (done): MC reads out-of-frame positions directly, killing the clamped scalar path; 1.4x on top of the NEON MC work | next speed items: dequant_8x8 in RDOQ, then the RD bit-cost tables |
| Lowres downscale | 2x2 box average | better decimation filter -- affects lookahead ME accuracy |
| Tunable constants | mb-tree strength 1.5 (Y264_MBTREE_STRENGTH env hook for sweeps; calibrated under CRF: 1.0/2.0/2.5 all flat-or-worse on mean, but 2.5 halves the static-content gap while costing motion clips -- content-adaptive strength is the real item); AQ strength 1.0 confirmed optimal under CRF (off +2.4%, 1.5 +4%); scene-cut thresh 0.40, min-keyint keyint/10; ABR smoothing 0.7/0.3 | content-adaptive mb-tree strength from the shot class; calibrate scene-cut/ABR against the corpus |

## Post-100%-parity: content-adaptive scaler selection

For the convex-hull ladder (shot-based plan stage 4) and any resolution-
switching mode: pick the downscale/upscale kernel per shot from content class
instead of one fixed filter. Sharp-edged synthetic content (anime, screen
capture, UI) keeps lines cleaner through kernels with less ringing and more
edge preservation (area/box down, EWA/spline or even NNEDI-class up), while
film/grain content favors Lanczos-3 down (matching Netflix/RCN-Hull
methodology) and grain-aware handling; soft/low-detail content tolerates
cheaper kernels. The stage-1 lookahead features already computed per shot
(edge energy vs grain energy: high-frequency variance that is spatially
structured vs temporally uncorrelated, plus the flat/animation content class
in stage 3) are the natural selector inputs. Evaluate per (kernel, class)
cell with the ladder harness: BD-rate on VMAF + VMAF-NEG (NEG matters here --
sharpening kernels game plain VMAF), decided on the bench corpus with anime /
screen-capture / film clips added. Also applies to the hull's quality-
comparison upscale (currently specced as fixed Lanczos-3): the upsampler used
for VMAF scoring should match what a player would do, or the hull tilts
toward the wrong rungs.

## Performance

**A matched per-stage ledger on samsung.** samsung is the one clip carrying
goal 1 (five of six others read 0.84-1.08x CRF), and a >5% single mechanism was
still hiding there after the corpus-wide decompositions called the area
exhausted. Build the ledger on BOTH sides for this one clip: x264's actual
per-stage spend on samsung (early-exit path lengths, how many MBs reach ME at
all, per-MB entropy cost on near-empty MBs, behavior across its multi-IDR
scene cuts) lined against ours stage by stage, instruction-counted so it is
load-immune. Every prior differential was corpus-wide or mechanism-first; the
remaining ~1.40x instruction ratio must live in a few stages of that table.
Start from the Y264_BPROF buckets (B ME 10.3% of wall, RD survivors 9.5%,
skip-eval 6.3%, direct RD 4.6%) and instrument x264 via the op-ledger patch
(scripts/op-ledger-x264.patch) extended with stage timers.

The hottest current cost is the RD loop measuring bits by actually CAVLC-coding
each candidate into a scratch bitstream (`eval_inter_part`, `eval_b_mode`,
`rdoq_4x4`). x264 avoids this with precomputed bit-cost tables. Replacing scratch
coding with a cost model is the single biggest encoder-speed win available.

| Item | Benefit | Effort | Status | Notes |
|---|---|---|---|---|
| CAVLC bit-cost tables for RD | speed L | M | idea | precompute level/run/coeff_token costs; drop scratch coding in the RD hot path |
| NEON deblock filter | speed M | M | idea | `filter_line` is scalar; vectorize the normal + strong filters |
| NEON 4x4 forward/inverse DCT | speed M | M | idea | quant/dequant are NEON; the transform butterflies are still C |
| Direct 8x8 / 16x16 SATD kernels | speed M | S | idea | mode decision sums 4x4 SATD; a fused 8x8 Hadamard is faster |
| SAD via i8mm/dotprod (UDOT) | speed M | S | idea | M-series has i8mm; motion search SAD maps well to dot-product |
| Half/quarter-pel MC in NEON | speed L | M | done | the 16x16-only NEON kernel takes any h<=16, and narrower widths compute 16-wide into a temp and copy out; mc_luma_c was ~75% of encode CPU after the sub-16 partition features landed; 2.2x whole-encoder speedup, byte-identical |
| Bit-estimate RDOQ (skip idct+cavlc per candidate) | speed M | M | idea | current RDOQ runs idct + CAVLC per trial level; a cost model is cheaper |
| Frame-level pipelining / lookahead thread | speed M | M | idea | overlap analysis with encode, beyond GOP-parallel; the t18 wait budget puts ~1.1 s of a 2.0 s t18 wall in the API-thread lookahead stages -- decoupling it (x264 runs a dedicated lookahead thread) is the next structural t18 lever, new risk class (lookahead state isolation), supervised build |
| mbtree srcA: hoist per-source build_lr_subpel into parallel units | speed S | M | idea | mbt_pa_source units cap at ~5x (t18 budget); the block loop chains MV predictors serially (decision-coupled, can't split) but the 2 per-source subpel builds are decision-neutral and parallelizable |
| Slice/tile parallelism within a frame | speed M | L | idea | complements GOP-parallel for low-latency / few-GOP cases |
| Lowres lookahead for scene-cut ME | speed M | M | idea | scene-cut runs a full-res per-MB diamond ME every frame; x264 does it on half-res lowres frames (~4x cheaper, larger effective search range). Downscale once, cache lowres + its MVs, reuse for adaptive-B and mb-tree |

## Lookahead and adaptive structure

The scene-cut detector and mb-tree already compute per-MB intra and best-inter
(real ME) costs on lowres. A proper lookahead generalises this to a *window of
future frames* and is the shared prerequisite for the items still open.

- **Sync-lookahead ring buffering (`done`, Y264_LA_BUF, default 0).**
 x264's --sync-lookahead does NOT shrink the window; it adds EXTRA input
 buffering in front of a window still held at full depth. Mapped onto
 yah264: grow the ring's CAPACITY to `la_depth + k` (`Y264_LA_BUF=k`) while
 every mb-tree/scene-cut walk stays capped at the original `la_depth`, so
 it's an ENGINEERING task gated on byte-identity, not a BD-rate call --
 confirmed by the gate itself (39/39 + a 36-cell deep sweep at every tested
 k, both mb-tree paths, determinism and TSan clean; one real off-by-one
 caught along the way, see docs/sync-lookahead-design.md). Measured with
 Y264_LA_THREAD combined (park_joy_720p 500f single-GOP, best-of-5): still
 flat, 0.99-1.01x at t18 and t8 -- the logical same-call coupling is
 genuinely gone, but wall-clock stays bounded by the la thread's chain work
 riding the SAME worker pool as the main analyze burst, which extra
 call-time slack can't free. Both envs stay default OFF.

- Lowres lookahead pass (`done`). Half-res (2x2 average) copy of each frame with a
 cheap 8x8 diamond ME against the previous frame, caching per-MB intra/inter
 costs and the winning MV. Scene-cut runs on it (~4x cheaper) and mb-tree reuses
 it. Still only cur-vs-prev; a multi-frame window is the next step.
- mb-tree (`done, windowed`). Mini-GOP B propagation plus anchor-chain
 propagation over the rc-lookahead window (bilinear splat, stops at IDRs);
 centred to zero mean so it redistributes rather than just spends bits. B
 configs only for now (IPPP offsets need the non-centred CRF/ABR model, see
 the sub-optimal table).
- Multi-frame lookahead window (`done`). Input delayed rc-lookahead
 frames (default 40) through a ring in front of the core; frames are analysed
 and scene cuts flagged at push time via a push-side replica of the since_idr
 state machine, so the core just obeys flags. Per-GOP thread determinism holds
 because propagation stops at IDRs. Unlocks b-adapt below.
- Adaptive B-frame placement (`done, bounded`). Frame typing moved
 to a one-push-lagged finalize on the window's push side, so the candidate B's
 *bidirectional* cost is measurable (the forward-only version was tried and
 reverted: on chaotic content it cost +7% / -2 dB). v1 rule is demote-only and
 conservative (bi-cost >= 0.9x intra), which in practice fires mainly on the
 dangling tail frames before an IDR/EOS (typed as proper reference anchors
 instead of non-ref P flushes); mid-stream demotion is largely pre-empted by
 the scene-cut IDR at ~0.6x intra forward cost. Note: with b-adapt on, the
 serial --dump-recon path and the per-GOP threaded path may type a GOP's last
 frame differently (the GOP instance sees no successor); same class of
 per-GOP shortcut as VBV.
- b-adapt structure-cost rule (`attempted twice, reverted`). Two
 formulations both over-demote and lose big: (1) pairwise STOP vs CONTINUE
 over raw lowres SATD sums, +8.4%/+8.9% BD even with the B term weighted
 0.75; (2) the faithful B_ADAPT_FAST geometry -- path costs "B^k PP" vs
 "B^k BP" from the last anchor, re-costing the pending run B's under each
 path's list-1 anchor and evaluating true averaged biprediction per MB --
 still +8.8%/+11.5%. Diagnosis: with integer-pel lowres ME the bi/fwd costs
 at distance are systematically overestimated relative to x264's half-pel
 refined, lambda-MV-costed lowres search, so P always looks relatively
 cheaper and every cost-driven rule demotes B's that are actually fine. The
 prerequisite is lowres search fidelity (half-pel + lambda MV costs + the
 intra lowres penalty), not another decision formula. A third attempt built
 the full fidelity stack (half-pel lowres planes, lambda-MV lowres ME,
 MV-field cache, temporal-direct bidir, lowres penalty, path-cost decision)
 and STILL lost (NEG +5.7/+16.4/+12.5%). The real finding: x264 medium itself
 keeps the FULL 3-B cadence on all three corpus clips (foreman 198B/mobile
 222B/akiyo 222B of 300f) and our conservative rule already reproduces that
 optimum -- so fixed 3-B is optimal here and ANY cost-driven deviation is
 pure downside. b-adapt cannot be measured to a win until variable-motion /
 partial-scene-cut / flash clips are added to the gate corpus; the blocker is
 the CORPUS, not the code. Typing machinery is in and gate-covered; the
 conservative demote rule ships meanwhile. Found and fixed on the way:
 without VUI max_num_reorder_frames, ffmpeg's adaptive reorder-depth
 heuristic silently drops a frame the first time a deeper mini-GOP follows
 shallower ones -- the SPS now signals the bitstream restriction.
| threads=1-vs-N bit-identity | correctness S | S | done | threads=1 (non-recon) routes through the GOP-parallel path too, so per-GOP frame_num/POC match at any thread count. The serial streaming path stays only for --dump-recon. Conformance asserts threads 1/2/8 byte-identity across baseline/cabac/8x8/bframes |

## Apple Silicon (M5 Max) specific

Runtime detect already reports FEAT_DotProd, i8mm, and SME on this hardware; only
NEON is used today. The matrix and GPU units are untapped.

- SME / SME2 for transforms and SATD (`idea`, benefit M, effort L). The M-series
 scalable matrix extension suits 4x4/8x8 transform and Hadamard SATD. Detect
 is already wired; needs SME kernels and a dispatch path.
- Metal GPU lookahead (`idea`, benefit M, effort L). Offload lookahead motion
 estimation and scene-cut detection to the GPU; unified memory makes the handoff
 cheap. This is the practical form of "GPU-assisted software encoding."
- GPU VMAF (`idea`, benefit S, effort M). Compute VMAF on the GPU for a
 VMAF-targeted rate-control mode without stalling the CPU encode.

## Compression tools (roadmap, become main features)

These are standard H.264 tools that close the gap to and past x264. Most are
already on the plan; listed here for the benefit/effort view.

- CABAC entropy coding (`done`, BD-rate ~10-15%, effort L). Universal win.
 I, P, and B slices are all conformant: recon-matches ffmpeg across QP 0-51,
 varied content, and bframes 1-3. Arithmetic engine, 460 context-init tables,
 residual coder, and slice plumbing all done; P adds mb_skip_flag, P mb_type,
 mvd (UEGk k=3) and the inter residual; B adds base-24 skip, the B mb_type
 binarization, B_Direct, dual-list mvd, and intra-in-B. Opt-in via --cabac;
 CAVLC is the default. Two classes of bug were found and fixed along the way:
 the long I-slice desync was two one-digit typos in the engine state tables
 (rangeTabLPS[31][0], transIdxLPS[28]) shared by encoder, decoder, and the
 test's reference decoder so everything round-tripped; and P/B needed the
 inter-specific coded_block_flag rules (unavailable-neighbour condTermFlag is 0
 for inter, and an inter MB with no residual must clear the chroma nnz too).
 Verified two ways: recon-match against ffmpeg, and an encoder-oplog-vs-ffmpeg
 bin trace (the definitive check when recon dump is unavailable). The bin-trace
 harness (scratchpad/yah264_log + instrumented ffmpeg) is reusable for future
 CABAC tools. There is also a CABAC decode engine (y264_cabac_dec_*) worth
 growing into a real decoder.
- 8x8 transform + Intra_8x8, High profile (`done`, BD-rate ~3-5%, effort M).
 Opt-in --transform-8x8. Normative 8x8 inverse + matched forward, six-category
 quant/dequant, Intra_8x8 (9 modes with the 8.3.2.2.1 reference filter),
 transform_size_8x8_flag, and the 8x8 luma residual for CAVLC (four interleaved
 4x4 sub-blocks) and CABAC (ctxBlockCat 5, no cbf, SIG8/LAST8 maps). Deblock
 skips the internal 4x4 edges of an 8x8 MB. Recon-matches ffmpeg I/P/B across
 QP 0-51. **Inter 8x8 transform** (`done`): P and B inter MBs choose
 4x4 vs 8x8 per MB by RD (real nC-context luma bits; the choice only affects the
 luma residual), with 8x8 RDOQ on the residual. Deblock derives the 8x8-block
 nonzero status by OR-ing the quadrant's per-4x4 nnz. Net -0.7..1.8% on foreman,
 -0.04..0.8% on mobile at equal PSNR; recon-matches ffmpeg (198/198).
- Multiple references (`done, partial`, BD-rate large on
 repetition/occlusion). CAVLC IPPP, per-MB reference (x264's default;
 --mixed-refs per-partition is the opt-in we skip). A most-recent-first ring of
 the N recent recons (default list-0 order, no reorder), ref_idx_l0 te(v),
 ref-aware MV prediction (mv_predict/partition_mvp take curref), deblock bS
 compares refIdx, sliding-window DPB. Recon-matches ffmpeg across ref 2-5, 8x8,
 AQ, crops (228/228). A per-partition different-ref predictor desynced on
 mixed-ref content, so partition 1 shares partition 0's ref (per-MB) -- exactly
 x264's non-mixed-refs behaviour. Remaining: CABAC ref_idx (ctxIdxOffset 54),
 B/b-pyramid multi-ref, mixed-refs, and WP-with-multi-ref (WP is disabled when
 nref>1 because the pred_weight_table is per-reference and our estimator is
 single-ref).
- Weighted prediction: implicit weighted biprediction (`done`, BD-rate small
 except fades, measured ~6-8% on a fade, effort S). weighted_bipred_idc 2,
 auto-on with B-frames; POC-distance weights (8.4.2.3.2) in B bi-pred and
 direct/skip, no signalling. Explicit P-slice WP (`done`, ~11-12% on a P fade):
 per-frame luma weight+offset from the src/ref DC ratio, pred_weight_table,
 applied in P luma MC; activates only on a detected fade. Weighted motion search
 (searching against the weighted reference for better MVs on fades) still an idea.
- B-pyramid / hierarchical B (`done`, ~1.3% at equal QP so far, effort L).
 Auto-on at bframes >= 2. General DPB with per-reference co-located motion,
 middle-first temporal-pyramid coding, running FrameNum, sliding window, P-list
 ref_pic_list_modification. Recon-matches ffmpeg (138/138). Per-temporal-layer
 QP offsets (ref B's lower QP) are the follow-up that grows the win; temporal
 direct (vs the current spatial direct) is still an `idea`.
- Trellis for CABAC (`idea`, BD-rate S-M, effort M). Extends the current CAVLC
 RDOQ once CABAC lands.

## Perceptual and content-aware (the differentiators)

This is where a new encoder earns its keep. All of these improve *perceived*
quality per bit, which raw PSNR/SSIM understate and VMAF partly captures.

- Adaptive quantization, variance-based (`done`, subjective win large, effort M).
 Spend bits on flat/low-variance areas where blocking is visible, pull them from
 busy areas. Shipped as --aq-strength: per-MB QP = frame QP + strength*(log2 MB
 variance - frame mean), centred on zero. Required the per-MB QP machinery
 (real mb_qp_delta coding + prediction chain, per-edge deblock QP per 8.7.2.2),
 which mb-tree and rate control also need. Conformant (recon-match with AQ on,
 CAVLC/CABAC, P/B) -- a latent bug where the RD trials perturbed the mb_qp_delta
 chain (broke AQ on P/B) was found and fixed once AQ entered the conformance gate.
 Next: psy-RD-style tuning and coupling AQ with rate control.
- Scene-cut detection (`done`, x264-style). Per-MB min(intra SATD-vs-DC, best
 inter from an integer diamond search vs the previous frame); cut when the frame
 P cost is not much below its intra cost (pcost >= (1-bias)*icost), bias adaptive
 on GOP length, with a min-keyint floor. The real motion search is essential --
 a zero-MV version conflated motion with cuts. +0.44-0.52 dB at ~+2% over a cut,
 no false positives on continuous motion. Follow-up: lowres frames for speed (it
 runs a full-res per-MB ME per frame now), and reuse the ME for adaptive B
 placement and mb-tree.
- Single-pass ABR rate control (`done`, --bitrate). Reactive controller: per-frame
 base QP from a smoothed complexity estimate C = bits*qscale plus a buffer-error
 correction, swing-limited. Tracks within ~1% on a single GOP; recon-matches and
 is thread-deterministic. Follow-ups: CRF (constant quality), VBV (peak-rate cap
 with a leaky-bucket buffer model -- needed for streaming), 2-pass, and carrying
 the controller across GOPs (conflicts with GOP-parallel independence; needs a
 lookahead-driven pre-pass or a shared rate state).
- MB-tree (`idea`, BD-rate large, effort M). Lower QP on macroblocks that many
 future frames reference, propagating quality backward. Big objective win and
 pairs naturally with our GOP structure.
- Psy-RD (`idea`, subjective, effort M). Bias mode decision toward preserving
 detail/energy rather than minimizing SSD, countering the blur that pure SSD RD
 produces.
- VMAF-targeted rate control (`idea`, effort M). Drive per-frame QP to hit a VMAF
 target rather than a bitrate, using the v1 model. Novel as a built-in.
- Per-shot / convex-hull encoding (`idea`, effort L). Detect shots, and per shot
 pick the resolution/QP operating point on the rate-distortion convex hull.
 Netflix per-title/per-shot, built into the encoder rather than an orchestrator.
 The single-pass version is the interesting one: predict the hull point from
 cheap lookahead features (VCA-style DCT energy, spatial/temporal complexity)
 instead of trial encodes. Netflix's dynamic optimizer needs dozens of encodes
 per shot; published ML hull-prediction work gets most of the gain from features
 alone. Output could be a whole ABR ladder per shot in one pass.
- Saliency-driven AQ (`idea`, subjective win M-L, effort M-L). A small saliency
 model (or a cheap motion-contrast heuristic first) modulates per-MB QP offsets
 on top of variance AQ. Recent VVC work (PAVEN, SJ-PVC) reports 7-22% bitrate
 savings at equal subjective quality from saliency plus JND. Even a crude
 version layered on variance AQ is worth testing against VMAF-NEG.
- Scene-cut detection for IDR/B placement (`idea`, effort S-M). Place keyframes at
 cuts and avoid B frames across them.
- Adaptive B-frame placement, b-adapt (`idea`, effort M). Decide per position
 whether a B frame helps; would fix the current high-motion B regression.

## Novel / research-flavored

Higher risk, higher differentiation. Worth prototyping once the fundamentals are solid.

- ML-guided mode pruning (`idea`, effort L). A tiny model predicts the likely-best
 modes per MB from cheap features, pruning the RD search. Speed win without the
 usual quality loss of heuristic early-exit.
- Content-adaptive lambda (`idea`, effort M). Tune the RD lambda per frame/region
 from complexity and temporal features instead of a fixed QP-derived value.
- Learned quantization offsets (`idea`, effort M). Replace the fixed deadzone /
 rounding offsets with content-learned ones.
- SME/GPU exhaustive-ME quality mode (`idea`, effort L). A "placebo+" tier that
 spends the matrix/GPU units on near-exhaustive motion search.
- Film grain synthesis via FGC SEI (`idea`, BD-rate large on grainy content,
 effort L). The film grain characteristics SEI *is* standard H.264 (Annex D
 since 2004; SMPTE RDD-5 defines the synthesis procedure) and FFmpeg's H.264
 decoder has applied it since 2021. Denoise the source, fit grain parameters
 (AR or frequency-filtering model), write the SEI. AV1-style grain synthesis,
 but no mainstream open H.264 encoder ships it. Playable today in FFmpeg-based
 players.
- Reference-frame temporal filtering (`idea`, BD-rate M on noisy content,
 effort M). Motion-compensated temporal filtering of a frame before encoding it
 as a reference, so references are cleaner and residuals shrink (SVT-AV1 does
 this for its alt-ref). Encode-side only, fully standard-compliant, and pairs
 with the grain SEI item (filter the reference, resynthesize the grain).
- Neural rate control / coded-size prediction (`idea`, effort M-L). Replace the
 heuristic bits model with a small learned predictor of coded size and
 distortion per MB or frame from lookahead features. Google shipped imitation-
 learned rate control and ML coded-size estimation in production encoders;
 accuracy gains show up directly as tighter VBV and less QP oscillation.
- Encode-aware neural prefilter (`idea`, effort L). Google's "sandwiched
 compression" wraps a standard codec in pre/post networks trained through a
 codec proxy. The decoder-agnostic half alone (a prefilter trained to make
 content cheaper for H.264 specifically, not a generic denoiser) is compatible
 with every existing player and reported meaningful gains. Train once offline,
 run per-frame on the ANE/GPU at encode time.
- Cross-rung analysis reuse for ABR ladders (`idea`, speed L for ladder use,
 effort M). When encoding the same content at several resolutions, encode the
 top rung, then scale and reuse its motion field, mode decisions, and shot
 metadata to seed the lower rungs. x264 has analysis dump/load as a niche
 feature; making the ladder a first-class single-invocation output is the
 differentiator, and it composes with the per-shot hull item above.
- Per-shot content classification for tool selection (`idea`, effort M). Classify
 each shot (animation, film grain, screen content, sports) from lookahead
 features and switch psy strength, deadzone, deblock offsets, and B-depth per
 shot. The per-shot machinery makes this nearly free once shots exist.

## Stream validation and conformance tooling (deferred to feature-complete)

Today `scripts/conformance.sh` does one kind of check: recon-match against a
single decoder (ffmpeg). ffmpeg is lenient and decodes technically-nonconformant
streams, so this misses whole classes of bugs. Build these out once the encoder
feature set is complete (they are additive to the existing gate). Priority order:

1. Second decoder oracle: openh264 (`idea`, effort S). Decode every conformance
 stream with ffmpeg *and* Cisco openh264, assert both match the encoder recon.
 Independent codebase catches encoder bugs ffmpeg forgives. `brew install
 openh264` for the lib; the `h264dec` CLI needs a source build.
2. Bitstream syntax/structure validator (`idea`, effort S-M). Parse NAL/SPS/PPS/
 slice headers and assert on the syntax elements (profile_idc, level_idc,
 transform_8x8_mode_flag, num_ref_frames, VUI). Catches the
 nonconformant-but-decodable class recon-match can't. Tool: h264bitstream's
 `h264_analyze` (build from source). mediainfo and gpac/MP4Box (already
 installed) give a coarser structural view usable as a cheap sanity layer now.
3. Level/profile conformance checker (`idea`, effort M). Annex A Table A-1 caps
 MB/s, frame size in MBs, DPB size, bitrate, CPB size, MV vertical range. Two
 validations: the encoder should *pick* the right level_idc from
 resolution/fps/bitrate (auto-level), and the stream must never *exceed* its
 declared level. No good off-the-shelf tool; write the checker against Table A-1.
 Relevant once RC and b-pyramid settle (both touch DPB size and bitrate).
4. HRD/VBV leaky-bucket verifier (`idea`, effort M). Simulate the CPB from the
 buffering_period/pic_timing SEI, assert no underflow/overflow. ffmpeg won't
 verify this; JM's ldecod has an HRD verifier, or write a ~100-line checker.
 plan.md's always-on list already names this; needed to back the VBV/2-pass work.
5. JM ldecod as the third, authoritative oracle (`idea`, effort M). The ITU/ISO
 reference decoder, definitive conformance authority. Build from JVT source
 (small C). Slow, so run on a reduced matrix in CI.
6. Generated parameter-coverage matrix (`idea`, effort S). Replace the
 hand-enumerated conformance cases with a generated sweep over
 {profile x entropy x bframes x 8x8 x AQ x RC-mode x geometry x QP x level}.

Also relevant, non-encoder: the official JVT/AVC conformance bitstreams (reference
streams + decoded MD5s) validate a *decoder* -- aimed at the `y264_cabac_dec_*`
engine if it grows into a real decoder. h26forge generates adversarial/spec-edge
H.264 for fuzzing the decode path.

## Implementation debt noticed while building

Small, concrete follow-ups from the current code:

- Intra I16-vs-I4 still uses a SATD threshold, not RD. Fold into the RD framework.
- RDOQ covers 4x4 (luma, chroma AC, I16 luma AC) and 8x8 (I_8x8 and inter 8x8).
 8x8 RDOQ is a no-op on intra-only 8x8 (I_8x8 is chosen on smooth blocks with
 nothing borderline to trim), but it is what makes the inter-8x8 transform a net
 win on detailed content: without it the plain-quant 8x8 loses to the RDOQ'd 4x4
 and the transform decision regresses mobile_cif; with it, mobile went from
 +0.6..1.4% to -0.04..0.8% and foreman to -0.7..1.8% at flat PSNR.
- RDOQ is greedy (trailing-elimination + level-down); a full Viterbi trellis gains more.
- B-frame deblocking (`done`). The bS derivation handles B's
 dual-list motion strength: different reference-picture set / MV
 count -> bS 1, else per-list MV-difference test; the L0/L1 pictures are distinct
 within a frame so the mixed-case pairing is unique). Deblock is enabled on every
 slice type; reference B's store their filtered recon into the DPB. Recon-matches
 ffmpeg across bframes 1-3, CAVLC/CABAC, 8x8, crops, QP 0-51.
- P_8x8 sub-partitions (sub_mb_type) not implemented; only 16x16/16x8/8x16.
- --dump-recon emits B frames in display order (a per-frame recon callback);
 the recon-match invariant works for B. Fixing that immediately exposed a B
 mv-predictor bug (a non-list / intra neighbour must contribute mv 0 to the
 median, 8.4.1.3.2), fixed in nb_at/nb_at_f. Lesson: without recon-match
 working for B, B reconstruction bugs stay invisible.
- mb_qp_delta is always coded as 0 (constant QP within a frame). Per-MB QP for
 variance AQ / MB-tree needs the real qp_delta coding + prediction chain.

## Quality knobs assessed vs x264, support or doc-why-not

Per owner directive: implement each remaining x264 knob unless it doesn't help
quality/performance, then document why. Results:

- **aq-mode 2**: already have it. Our AQ centres per-MB log2(var) on the frame
 mean (auto-variance). **aq-mode 3** (dark bias) hook added (Y264_AQ_DARK) but
 OFF: negligible on the (bright) corpus; needs dark/banding clips + subjective
 eval. [SHIPPED hook / documented]
- **Tunable deadzone** (x264 --deadzone-inter/intra): NOT worth a knob for us.
 Our naive quant uses a fixed rounding offset f=(1<<qbits)/(3 intra,6 inter),
 but **bidirectional RDOQ re-optimizes the rounding per-block by RD**: its
 away-from-zero probe explicitly recovers deadzone-suppressed coefficients
 (measured -0.36/-0.93/-0.31% BD-PSNR when added). A global deadzone constant
 can't beat per-block RD, so exposing it would only affect RDOQ's start point.
 [DOC why-not: subsumed by RDOQ]
- **weightp-2** (per-ref/region weighted P): we have frame-level DC weighted P
 (fade detection, one weight per ref) + implicit bi-pred weights. Per-ref/
 region WP is a refinement whose gain shows only on multi-ref fades/flashes,
 none in the gate corpus, so it is unmeasurable here. [DOC why-not:
 corpus-limited; revisit with a fade/flash corpus. Real remaining WP gap, but
 small.]
- **RC knobs**: **ipratio/pbratio are implemented** as the temporal-layer QP
 cascade (I lower ~1.4, referenced-B higher by depth). qpmin/qpmax/
 chroma-qp-offset are user-control clamps with sane internal defaults, not
 quality/perf levers. [DOC: expose as CLI params for API parity on demand; no
 quality impact, not a blocker for the optimization phase.]

Net: no shippable quality win among these; all either already present, subsumed
by RDOQ, corpus-unmeasurable, or user-control-only. The feature set is settled
for the optimization phase (the one real structural item left is 10-bit, see
high-bit-depth-plan.md).

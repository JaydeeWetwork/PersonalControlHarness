# PCH test beds as CTest tests. Python drives them; exit code 77 = SKIPPED
# (a missing prerequisite or a PCH feature that does not exist yet).
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(PCH_HARNESS_DIR "${PROJECT_SOURCE_DIR}/pch/harness")
set(_env "PCH_HARNESS_DIR=${PCH_HARNESS_DIR}" "PYTHONDONTWRITEBYTECODE=1")

function(pch_testbed name script)
  add_test(NAME ${name}
           COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/${script}" ${ARGN}
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(${name} PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "${_env}"
    LABELS "pch;testbed")
endfunction()

# Pure-Python, run today (no torch needed)
pch_testbed(pch.shipped_streams_chain    tb_shipped_streams_chain.py)
pch_testbed(pch.receipt_tamper           tb_receipt_tamper.py)
pch_testbed(pch.receipt_tamper_fuzz      tb_receipt_tamper.py --fuzz 10000)
pch_testbed(pch.trailer_in_chain         tb_trailer_chain.py)
pch_testbed(pch.sig_golden_vectors       tb_sig_vectors.py)
pch_testbed(pch.sign_at_close            tb_sign_close.py)
pch_testbed(pch.verify_cli_offline       tb_verify_cli.py)
pch_testbed(pch.signature_fuzz           tb_signature_fuzz.py 10000)
pch_testbed(pch.vendored_specimen        tb_vendored_specimen.py)
pch_testbed(pch.weights_path             tb_weights_path.py)
pch_testbed(pch.weights_local            tb_weights_path.py --local)
pch_testbed(pch.review_regressions       tb_review_fixes.py)
pch_testbed(pch.docs_check               ../tools/check_docs.py)
pch_testbed(pch.upstream_drift           ../tools/check_upstream_drift.py)
pch_testbed(pch.key_rotation             tb_key_rotation.py)
pch_testbed(pch.boss.stub_conformance     tb_stub_conformance.py)

# Need the reference model (torch + DeepSeek-V3 specimen); skip cleanly without it
pch_testbed(pch.verify_shipped_streams   run_harness_script.py verify_streams.py)
pch_testbed(pch.disc_test                run_harness_script.py disc_test_llm.py)
pch_testbed(pch.battery2_rewind_witness  run_harness_script.py pch_more_tests.py)
pch_testbed(pch.replay_determinism       tb_model_testbeds.py replay_determinism)
pch_testbed(pch.thread_determinism       tb_model_testbeds.py thread_determinism)
pch_testbed(pch.rewind_fidelity          tb_model_testbeds.py rewind_fidelity)
pch_testbed(pch.fork_budget              tb_model_testbeds.py fork_budget)
set_tests_properties(pch.fork_budget pch.rewind_fidelity PROPERTIES TIMEOUT 3600 LABELS "pch;testbed;slow")

# Boss / fair-play beds 08-14 run on the PCH reference boss stub
# (pch/harness/boss.py, a documented minimal stub). Owner decision for every
# bed: testbeds/DECISIONS.md.
pch_testbed(pch.boss.versioned_rematch tb_boss_fixture.py versioned_rematch)
pch_testbed(pch.boss.human_input_rate tb_boss_fixture.py human_input_rate)
pch_testbed(pch.boss.ranked_fixed_branch_count tb_boss_fixture.py ranked_fixed_branch_count)
pch_testbed(pch.boss.brain_version_stamp tb_boss_fixture.py brain_version_stamp)
pch_testbed(pch.boss.hud_reading tb_boss_fixture.py hud_reading)
pch_testbed(pch.boss.telegraph_fairness tb_boss_fixture.py telegraph_fairness)
pch_testbed(pch.boss.hud_only_honesty tb_boss_fixture.py hud_only_honesty)

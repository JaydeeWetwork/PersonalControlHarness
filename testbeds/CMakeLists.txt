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

# Need the reference model (torch + DeepSeek-V3 specimen); skip cleanly without it
pch_testbed(pch.verify_shipped_streams   run_harness_script.py verify_streams.py)
pch_testbed(pch.disc_test                run_harness_script.py disc_test_llm.py)
pch_testbed(pch.battery2_rewind_witness  run_harness_script.py pch_more_tests.py)
pch_testbed(pch.replay_determinism       tb_model_testbeds.py replay_determinism)
pch_testbed(pch.thread_determinism       tb_model_testbeds.py thread_determinism)
pch_testbed(pch.rewind_fidelity          tb_model_testbeds.py rewind_fidelity)
pch_testbed(pch.fork_budget              tb_model_testbeds.py fork_budget)
set_tests_properties(pch.fork_budget pch.rewind_fidelity PROPERTIES TIMEOUT 3600 LABELS "pch;testbed;slow")

# Boss / fair-play designs: no boss brain in the public PCH yet -> skipped with reason
foreach(tb hud_reading telegraph_fairness versioned_rematch human_input_rate ranked_fixed_branch_count brain_version_stamp hud_only_honesty)
  pch_testbed(pch.boss.${tb} tb_boss_pending.py ${tb})
endforeach()

# test.py (或 debug_bindings.py) - 修正版
import sys
import pathlib
from wangcai_bt import TradeCallback, OrderCallback
# ----------------------------------------

print(f"Inspecting classes from module: wangcai_bt")
print("-" * 40)

# --- 诊断 TradeCallback ---
print("\n[INFO] Diagnosing TradeCallback...")
try:
    # 修正：直接调用 TradeCallback，而不是 wangcai_bt.TradeCallback
    trade_cb_instance = TradeCallback(
        "test_id_1", 'B', 100, 12345, 123.45, 100, "2025-08-15 09:30:00", 'T'
    )
    print("[SUCCESS] TradeCallback object created.")
    
    attributes = dir(trade_cb_instance)
    print("[ATTRIBUTES] Found on TradeCallback object:")
    print(attributes)
    
    if 'localid' in attributes:
        print("\n[SUCCESS] 'localid' attribute is present!")
    else:
        print("\n[FAILURE] 'localid' attribute is MISSING!")

except Exception as e:
    print(f"[ERROR] Could not create or inspect TradeCallback: {e}")

print("-" * 40)

# --- 诊断 OrderCallback ---
print("\n[INFO] Diagnosing OrderCallback...")
try:
    # 修正：直接调用 OrderCallback
    order_cb_instance = OrderCallback(
        "2025-08-15 09:31:00", 1, 12345, 12300, 100, 12310, 100, 1, "test_id_2"
    )
    print("[SUCCESS] OrderCallback object created.")
    
    attributes = dir(order_cb_instance)
    print("[ATTRIBUTES] Found on OrderCallback object:")
    print(attributes)

    if 'orderlocalid' in attributes:
        print("\n[SUCCESS] 'orderlocalid' attribute is present!")
    else:
        print("\n[FAILURE] 'orderlocalid' attribute is MISSING!")
        
    if 'localid' in attributes:
        print("[SUCCESS] 'localid' alias attribute is present!")
    else:
        print("[FAILURE] 'localid' alias attribute is MISSING!")

except Exception as e:
    print(f"[ERROR] Could not create or inspect OrderCallback: {e}")
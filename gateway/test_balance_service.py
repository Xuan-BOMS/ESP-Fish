import io
import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault("DEEPSEEK_API_KEY", "test-key")
sys.path.insert(0, str(Path(__file__).resolve().parent))
import balance_service


class BalanceServiceTest(unittest.TestCase):
    def test_fetches_and_normalizes_balance_response(self):
        response = io.BytesIO(json.dumps({
            "is_available": True,
            "balance_infos": [
                {"currency": "CNY", "total_balance": "12.34", "granted_balance": "0", "topped_up_balance": "12.34"}
            ],
        }).encode())
        with patch.object(balance_service, "urlopen", return_value=response) as fetch:
            balances, stale = balance_service.refresh_balance()
        self.assertEqual(balances, [{"currency": "CNY", "total_balance": "12.34"}])
        self.assertFalse(stale)
        request = fetch.call_args.args[0]
        self.assertEqual(request.full_url, "https://api.deepseek.com/user/balance")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-key")


if __name__ == "__main__":
    unittest.main()

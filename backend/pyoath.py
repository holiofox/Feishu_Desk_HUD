import requests
import json
import os

url = "https://open.feishu.cn/open-apis/authen/v2/oauth/token"
headers = {"Content-Type": "application/json"}

data = {
    "grant_type": "authorization_code",
    # [请修改] 替换为你的飞书 App ID
    "client_id": os.getenv("FEISHU_APP_ID", "YOUR_APP_ID"),
    # [请修改] 替换为你的飞书 App Secret
    "client_secret": os.getenv("FEISHU_APP_SECRET", "YOUR_APP_SECRET"),
    # [请修改] 替换为实际获取到的 Authorization Code
    "code": "YOUR_AUTHORIZATION_CODE",
    "redirect_uri": "https://open.feishu.cn/api-explorer/loading"
}

response = requests.post(url, headers=headers, json=data)
result = response.json()
print(json.dumps(result, indent=2, ensure_ascii=False))
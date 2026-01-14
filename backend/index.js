// ===================== 配置模块 =====================
const express = require('express');
const mqtt = require('mqtt');
const lark = require('@larksuiteoapi/node-sdk');
const fs = require('fs');
require('dotenv').config();

const config = {
  feishu: {
    appId: process.env.FEISHU_APP_ID,
    appSecret: process.env.FEISHU_APP_SECRET,
    userAccessToken: process.env.USER_ACCESS_TOKEN,
  },
  emqx: {
    // 替换为你的 EMQX 服务器地址
    broker: 'mqtts://your-emqx-server-address:8883',
    username: process.env.EMQX_USERNAME || '',
    password: process.env.EMQX_PASSWORD || '',
    topic: process.env.EMQX_TOPIC || '',
    clientId: `feishu_bridge_${Math.random().toString(16).slice(3)}`,
    ca: process.env.EMQX_CA_PATH || '',
  },
  port: process.env.PORT || 3000,
  syncInterval: parseInt(process.env.SYNC_INTERVAL || '0'),
};

// ===================== Token管理模块 =====================
let tokenStore = {
  userAccessToken: process.env.USER_ACCESS_TOKEN || '',
  refreshToken: process.env.REFRESH_TOKEN || '',
  expiresAt: Date.now() + 7200 * 1000, // 默认2小时后过期
  refreshExpiresAt: Date.now() + (parseInt(process.env.REFRESH_TOKEN_EXPIRES_IN || '604800') * 1000),
};

/**
 * 刷新 user_access_token
 */
async function refreshUserAccessToken() {
  try {
    console.log('\n🔄 开始刷新 user_access_token...');
    
    if (!tokenStore.refreshToken) {
      throw new Error('未设置 REFRESH_TOKEN,无法刷新');
    }

    const response = await fetch('https://open.feishu.cn/open-apis/authen/v2/oauth/token', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json; charset=utf-8',
      },
      body: JSON.stringify({
        grant_type: 'refresh_token',
        client_id: config.feishu.appId,
        client_secret: config.feishu.appSecret,
        refresh_token: tokenStore.refreshToken,
      }),
    });

    const data = await response.json();
    
    if (data.code !== 0) {
      const errorMsg = data.error_description || data.error || `错误码: ${data.code}`;
      throw new Error(`刷新token失败: ${errorMsg}`);
    }

    // 更新token存储
    tokenStore.userAccessToken = data.access_token;
    tokenStore.refreshToken = data.refresh_token;
    tokenStore.expiresAt = Date.now() + (data.expires_in * 1000);
    tokenStore.refreshExpiresAt = Date.now() + (data.refresh_token_expires_in * 1000);

    // 更新配置
    config.feishu.userAccessToken = data.access_token;

    console.log('✓ Token刷新成功');
    console.log(`  新token将在 ${new Date(tokenStore.expiresAt).toLocaleString('zh-CN')} 过期`);
    console.log(`  Refresh token将在 ${new Date(tokenStore.refreshExpiresAt).toLocaleString('zh-CN')} 过期`);
    
    // 保存到文件以便重启后使用
    saveTokensToFile();
    
    return true;
  } catch (error) {
    console.error('✗ 刷新token失败:', error.message);
    
    // 检查常见错误码
    if (error.message.includes('20037')) {
      console.error('\n⚠️  Refresh token已过期!需要重新授权');
      console.error('请访问: https://open.feishu.cn/api-explorer/');
    } else if (error.message.includes('20064') || error.message.includes('20073')) {
      console.error('\n⚠️  Refresh token已被撤销或已使用!需要重新授权');
      console.error('请访问: https://open.feishu.cn/api-explorer/');
    } else if (error.message.includes('20010')) {
      console.error('\n⚠️  用户无应用使用权限,请检查权限配置');
    }
    
    throw error;
  }
}

/**
 * 检查并自动刷新token
 */
async function checkAndRefreshToken() {
  const now = Date.now();
  const timeUntilExpiry = tokenStore.expiresAt - now;
  
  // 提前5分钟刷新
  if (timeUntilExpiry < 5 * 60 * 1000) {
    console.log('⏰ Token即将过期,开始自动刷新...');
    await refreshUserAccessToken();
  }
}

/**
 * 保存token到文件
 */
function saveTokensToFile() {
  try {
    const tokenData = {
      userAccessToken: tokenStore.userAccessToken,
      refreshToken: tokenStore.refreshToken,
      expiresAt: tokenStore.expiresAt,
      refreshExpiresAt: tokenStore.refreshExpiresAt,
      updatedAt: new Date().toISOString(),
    };
    
    fs.writeFileSync('.tokens.json', JSON.stringify(tokenData, null, 2));
    console.log('✓ Token已保存到 .tokens.json');
  } catch (error) {
    console.warn('⚠️  保存token文件失败:', error.message);
  }
}

/**
 * 从文件加载token
 */
function loadTokensFromFile() {
  try {
    if (fs.existsSync('.tokens.json')) {
      const data = JSON.parse(fs.readFileSync('.tokens.json', 'utf8'));
      
      // 检查refresh_token是否过期
      if (data.refreshExpiresAt > Date.now()) {
        tokenStore = data;
        config.feishu.userAccessToken = data.userAccessToken;
        console.log('✓ 从文件加载token成功');
        
        // 如果access_token已过期但refresh_token未过期,立即刷新
        if (data.expiresAt <= Date.now()) {
          console.log('⚠️  Access token已过期,立即刷新...');
          // 异步刷新,不阻塞启动
          refreshUserAccessToken().catch(err => {
            console.error('启动时刷新token失败:', err.message);
          });
        }
        
        return true;
      } else {
        console.log('⚠️  文件中的refresh_token已过期,需要重新授权');
      }
    }
  } catch (error) {
    console.warn('⚠️  加载token文件失败:', error.message);
  }
  return false;
}

// ===================== 飞书模块 =====================
const feishuClient = new lark.Client({
  appId: config.feishu.appId,
  appSecret: config.feishu.appSecret,
});

/**
 * 获取飞书任务列表
 */
async function getFeishuTasks() {
  // 每次调用前检查token
  await checkAndRefreshToken();
  
  const token = tokenStore.userAccessToken;
  if (!token) throw new Error('未设置 USER_ACCESS_TOKEN');
  
  try {
    const response = await feishuClient.task.v2.task.list(
      {
        params: {
          page_size: 50,
          type: 'my_tasks',
          user_id_type: 'open_id',
        },
      },
      lark.withUserAccessToken(token)
    );
    
    if (response.code !== 0) {
      // 检查是否是token过期错误
      if (response.code === 99991663 || response.code === 99991661) {
        console.log('⚠️  Token无效或过期,尝试刷新...');
        await refreshUserAccessToken();
        // 重试请求
        return getFeishuTasks();
      }
      throw new Error(`飞书API返回错误: ${response.msg || '未知错误'} (code: ${response.code})`);
    }
    
    return response.data?.items || [];
  } catch (error) {
    // 处理token过期情况
    if (error.message.includes('token') || error.message.includes('unauthorized') || error.message.includes('99991663')) {
      console.log('⚠️  检测到token问题,尝试刷新...');
      try {
        await refreshUserAccessToken();
        // 只重试一次
        const response = await feishuClient.task.v2.task.list(
          {
            params: {
              page_size: 50,
              type: 'my_tasks',
              user_id_type: 'open_id',
            },
          },
          lark.withUserAccessToken(tokenStore.userAccessToken)
        );
        
        if (response.code !== 0) {
          throw new Error(`飞书API返回错误: ${response.msg || '未知错误'} (code: ${response.code})`);
        }
        
        return response.data?.items || [];
      } catch (retryError) {
        console.error('重试失败:', retryError.message);
        throw retryError;
      }
    }
    throw error;
  }
}

// ===================== MQTT模块 =====================
let mqttClient = null;

/**
 * 连接到EMQX
 */
function connectMQTT() {
  const options = {
    clientId: config.emqx.clientId,
    username: config.emqx.username,
    password: config.emqx.password,
    clean: true,
    reconnectPeriod: 5000,
    connectTimeout: 30 * 1000,
  };
  
  if (fs.existsSync(config.emqx.ca)) {
    options.ca = fs.readFileSync(config.emqx.ca);
    options.rejectUnauthorized = true;
  } else {
    console.warn('⚠️  警告: CA证书文件不存在,将使用不安全的连接');
    options.rejectUnauthorized = false;
  }
  
  mqttClient = mqtt.connect(config.emqx.broker, options);

  mqttClient.on('connect', () => {
    console.log('✓ 已连接到EMQX服务器');
    console.log(`  地址: ${config.emqx.broker}`);
    console.log(`  客户端ID: ${config.emqx.clientId}`);
  });
  
  mqttClient.on('error', err => console.error('✗ MQTT连接错误:', err.message));
  mqttClient.on('reconnect', () => console.log('⟳ 正在重新连接到EMQX...'));
  mqttClient.on('offline', () => console.log('⚠ MQTT客户端离线'));
}

/**
 * 发布消息到EMQX
 */
function publishToEMQX(topic, message) {
  return new Promise((resolve, reject) => {
    if (!mqttClient || !mqttClient.connected) {
      return reject(new Error('MQTT客户端未连接'));
    }
    const payload = JSON.stringify(message);
    // 使用 Retain 确保设备上线即可收到最新的全量列表
    mqttClient.publish(topic, payload, { qos: 1, retain: true }, err => {
      if (err) {
        console.error('✗ 发布消息失败:', err.message);
        reject(err);
      } else {
        // console.log(`✓ 消息已发布到主题: ${topic}`); // 减少日志刷屏
        resolve();
      }
    });
  });
}

// ===================== 任务同步模块 =====================
/**
 * 获取并发布飞书任务
 * 修改内容：
 * 1. 过滤：只保留 status === 'todo'
 * 2. 排序：按截止时间升序，无时间排最后
 * 3. 结构：改为一次性发送 JSON 数组
 */
async function fetchAndPublishTasks() {
  try {
    console.log('\n========== 开始同步飞书任务 ==========');
    console.log(`时间: ${new Date().toLocaleString('zh-CN')}`);
    
    // 1. 获取所有任务
    const allTasks = await getFeishuTasks();
    console.log(`从飞书获取到 ${allTasks.length} 个原始任务`);
    
    // 2. 过滤：只保留待办 (todo) 任务
    let todoTasks = allTasks.filter(task => task.status === 'todo');
    
    // 3. 排序：按截止时间从近到远
    // 逻辑：timestamp 越小代表时间越早。
    // 如果没有 due 或者 timestamp，则设为最大整数，排在最后。
    todoTasks.sort((a, b) => {
      const timeA = (a.due && a.due.timestamp) ? Number(a.due.timestamp) : Number.MAX_SAFE_INTEGER;
      const timeB = (b.due && b.due.timestamp) ? Number(b.due.timestamp) : Number.MAX_SAFE_INTEGER;
      return timeA - timeB;
    });

    console.log(`筛选并排序后，剩余 ${todoTasks.length} 个待办任务`);
    
    // 4. 构建全量数组 (Payload Array)
    const payload = todoTasks.map(task => ({
        taskId: task.guid,
        summary: task.summary,
        description: task.description,
        status: task.status,
        createdAt: task.created_at,
        updatedAt: task.updated_at,
        completedAt: task.completed_at,
        dueTimestamp: task.due?.timestamp,
        dueIsAllDay: task.due?.is_all_day,
    }));

    // 5. 一次性发布整个数组
    await publishToEMQX(`${config.emqx.topic}/tasks`, payload);
    
    if (payload.length > 0) {
        console.log(`  - 首个任务: ${payload[0].summary}`);
    }
    console.log(`✓ 已成功发布 ${todoTasks.length} 个待办任务列表(JSON数组)到EMQX`);
    console.log('======================================\n');
    
    return { success: true, count: todoTasks.length };
  } catch (error) {
    console.error('✗ 同步任务失败:', error.message);
    
    if (error.message.includes('token') || error.message.includes('unauthorized')) {
      console.error('\n⚠️  Token可能已过期或无效！');
      console.error('系统将尝试自动刷新,如果持续失败请重新授权');
      console.error('访问: https://open.feishu.cn/api-explorer/\n');
    }
    
    throw error;
  }
}

// ===================== Express服务模块 =====================
const app = express();
app.use(express.json());

// 同步任务接口
app.post('/sync/tasks', async (req, res) => {
  try {
    const result = await fetchAndPublishTasks();
    res.json(result);
  } catch (error) {
    res.status(500).json({ success: false, error: error.message });
  }
});

// 手动发布消息接口
app.post('/publish', async (req, res) => {
  const { topic, message } = req.body;
  if (!topic || !message) {
    return res.status(400).json({ error: '缺少 topic 或 message 参数' });
  }
  try {
    await publishToEMQX(topic, message);
    res.json({ success: true, message: '消息发布成功' });
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// 手动刷新token接口
app.post('/refresh/token', async (req, res) => {
  try {
    await refreshUserAccessToken();
    res.json({ 
      success: true, 
      message: 'Token刷新成功',
      expiresAt: new Date(tokenStore.expiresAt).toLocaleString('zh-CN'),
      refreshExpiresAt: new Date(tokenStore.refreshExpiresAt).toLocaleString('zh-CN')
    });
  } catch (error) {
    res.status(500).json({ success: false, error: error.message });
  }
});

// Token状态查询接口
app.get('/token/status', (req, res) => {
  const now = Date.now();
  const expiresIn = Math.floor((tokenStore.expiresAt - now) / 1000);
  const refreshExpiresIn = Math.floor((tokenStore.refreshExpiresAt - now) / 1000);
  
  res.json({
    hasToken: !!tokenStore.userAccessToken,
    hasRefreshToken: !!tokenStore.refreshToken,
    expiresIn: expiresIn > 0 ? expiresIn : 0,
    expiresAt: new Date(tokenStore.expiresAt).toLocaleString('zh-CN'),
    isExpired: expiresIn <= 0,
    refreshExpiresIn: refreshExpiresIn > 0 ? refreshExpiresIn : 0,
    refreshExpiresAt: new Date(tokenStore.refreshExpiresAt).toLocaleString('zh-CN'),
    refreshIsExpired: refreshExpiresIn <= 0,
  });
});

// 健康检查接口
app.get('/health', (req, res) => {
  const now = Date.now();
  const tokenExpiresIn = Math.floor((tokenStore.expiresAt - now) / 1000);
  
  res.json({
    status: 'ok',
    mqtt: mqttClient?.connected ? 'connected' : 'disconnected',
    hasUserToken: !!tokenStore.userAccessToken,
    hasRefreshToken: !!tokenStore.refreshToken,
    tokenExpiresIn: tokenExpiresIn > 0 ? tokenExpiresIn : 0,
    tokenExpired: tokenExpiresIn <= 0,
    autoSync: config.syncInterval > 0,
    timestamp: Date.now(),
  });
});

// 首页 - 显示使用说明
app.get('/', (req, res) => {
  const now = Date.now();
  const tokenExpiresIn = Math.floor((tokenStore.expiresAt - now) / 1000);
  const refreshExpiresIn = Math.floor((tokenStore.refreshExpiresAt - now) / 1000);
  
  res.send(`
    <html>
      <head>
        <title>飞书-EMQX消息桥接服务</title>
        <meta charset="UTF-8">
        <style>
          body { 
            font-family: 'Segoe UI', Arial, sans-serif; 
            max-width: 900px; 
            margin: 50px auto; 
            padding: 20px; 
            background: #f8f9fa;
          }
          h1 { color: #2c3e50; border-bottom: 3px solid #3498db; padding-bottom: 10px; }
          h2 { color: #34495e; margin-top: 30px; }
          .section { 
            margin: 20px 0; 
            padding: 20px; 
            background: white; 
            border-radius: 8px; 
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
          }
          .status { 
            display: inline-block; 
            padding: 4px 12px; 
            border-radius: 12px; 
            font-size: 14px;
            font-weight: bold;
          }
          .status.ok { background: #d4edda; color: #155724; }
          .status.error { background: #f8d7da; color: #721c24; }
          .status.warning { background: #fff3cd; color: #856404; }
          code { 
            background: #f4f4f4; 
            padding: 3px 8px; 
            border-radius: 4px; 
            font-family: 'Courier New', monospace;
            color: #e83e8c;
          }
          button {
            background: #3498db;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 5px;
            cursor: pointer;
            font-size: 14px;
            margin: 5px;
          }
          button:hover { background: #2980b9; }
          button.warning { background: #f39c12; }
          button.warning:hover { background: #e67e22; }
          .api-list { list-style: none; padding: 0; }
          .api-list li { 
            padding: 10px; 
            margin: 8px 0; 
            background: #f8f9fa; 
            border-left: 4px solid #3498db;
            border-radius: 4px;
          }
          .method { 
            display: inline-block; 
            padding: 2px 8px; 
            background: #28a745; 
            color: white; 
            border-radius: 3px; 
            font-size: 12px; 
            margin-right: 10px; 
          }
          .method.post { background: #ffc107; }
          .timer { font-size: 12px; color: #666; }
        </style>
      </head>
      <body>
        <h1>🚀 飞书-EMQX消息桥接服务</h1>
        
        <div class="section">
          <h2>📊 服务状态</h2>
          <p>
            MQTT连接: 
            <span class="status ${mqttClient?.connected ? 'ok' : 'error'}">
              ${mqttClient?.connected ? '✓ 已连接' : '✗ 未连接'}
            </span>
          </p>
          <p>
            Access Token: 
            <span class="status ${tokenStore.userAccessToken ? (tokenExpiresIn > 0 ? 'ok' : 'error') : 'error'}">
              ${tokenStore.userAccessToken ? (tokenExpiresIn > 0 ? `✓ 有效 (${Math.floor(tokenExpiresIn / 60)}分钟后过期)` : '✗ 已过期') : '✗ 未配置'}
            </span>
          </p>
          <p>
            Refresh Token: 
            <span class="status ${tokenStore.refreshToken ? (refreshExpiresIn > 0 ? 'ok' : 'error') : 'error'}">
              ${tokenStore.refreshToken ? (refreshExpiresIn > 0 ? `✓ 有效 (${Math.floor(refreshExpiresIn / 3600 / 24)}天后过期)` : '✗ 已过期') : '✗ 未配置'}
            </span>
          </p>
          <p>
            自动同步: 
            <span class="status ${config.syncInterval > 0 ? 'ok' : 'warning'}">
              ${config.syncInterval > 0 ? `✓ 每${config.syncInterval}秒` : '⚠ 未启用'}
            </span>
          </p>
        </div>
        
        <div class="section">
          <h2>🎯 快速操作</h2>
          <button onclick="syncTasks()">立即同步任务</button>
          <button onclick="checkHealth()">健康检查</button>
          <button onclick="checkTokenStatus()">Token状态</button>
          <button class="warning" onclick="refreshToken()">手动刷新Token</button>
          <div id="result" style="margin-top: 15px; padding: 10px; background: #f8f9fa; border-radius: 4px; display: none;"></div>
        </div>
        
        <div class="section">
          <h2>🔧 API接口列表</h2>
          <ul class="api-list">
            <li>
              <span class="method">GET</span>
              <code>/health</code> - 健康检查
            </li>
            <li>
              <span class="method">GET</span>
              <code>/token/status</code> - Token状态查询
            </li>
            <li>
              <span class="method post">POST</span>
              <code>/sync/tasks</code> - 同步飞书任务
            </li>
            <li>
              <span class="method post">POST</span>
              <code>/refresh/token</code> - 手动刷新Token
            </li>
            <li>
              <span class="method post">POST</span>
              <code>/publish</code> - 手动发布消息到EMQX
              <br><small style="margin-left: 60px;">参数: {"topic": "主题", "message": {数据}}</small>
            </li>
          </ul>
        </div>
        
        <div class="section">
          <h2>📋 配置信息</h2>
          <p><strong>MQTT主题:</strong> <code>${config.emqx.topic}</code></p>
          <p><strong>任务主题:</strong> <code>${config.emqx.topic}/tasks</code></p>
          <p><strong>服务端口:</strong> <code>${config.port}</code></p>
          <p><strong>Token过期时间:</strong> <code>${new Date(tokenStore.expiresAt).toLocaleString('zh-CN')}</code></p>
        </div>
        
        <script>
          function showResult(msg, isError = false, isWarning = false) {
            const div = document.getElementById('result');
            div.style.display = 'block';
            if (isError) {
              div.style.background = '#f8d7da';
              div.style.color = '#721c24';
            } else if (isWarning) {
              div.style.background = '#fff3cd';
              div.style.color = '#856404';
            } else {
              div.style.background = '#d4edda';
              div.style.color = '#155724';
            }
            div.innerHTML = msg;
          }
          
          async function syncTasks() {
            try {
              showResult('正在同步任务...');
              const res = await fetch('/sync/tasks', { method: 'POST' });
              const data = await res.json();
              if (data.success) {
                showResult('✓ 成功同步 ' + data.count + ' 个任务');
              } else {
                showResult('✗ ' + data.error, true);
              }
            } catch (e) {
              showResult('✗ 请求失败: ' + e.message, true);
            }
          }
          
          async function checkHealth() {
            try {
              const res = await fetch('/health');
              const data = await res.json();
              const msg = '健康检查结果:<br>' + 
                'MQTT: ' + data.mqtt + '<br>' +
                'Token: ' + (data.hasUserToken ? '已配置' : '未配置') + '<br>' +
                'Token过期: ' + (data.tokenExpired ? '是' : '否') + '<br>' +
                'Token剩余: ' + Math.floor(data.tokenExpiresIn / 60) + '分钟';
              showResult(msg, data.tokenExpired);
            } catch (e) {
              showResult('✗ 请求失败: ' + e.message, true);
            }
          }
          
          async function checkTokenStatus() {
            try {
              const res = await fetch('/token/status');
              const data = await res.json();
              const msg = 'Token状态:<br>' + 
                'Access Token: ' + (data.isExpired ? '已过期' : '有效 (' + Math.floor(data.expiresIn / 60) + '分钟)') + '<br>' +
                'Refresh Token: ' + (data.refreshIsExpired ? '已过期' : '有效 (' + Math.floor(data.refreshExpiresIn / 3600 / 24) + '天)') + '<br>' +
                'Access过期时间: ' + data.expiresAt + '<br>' +
                'Refresh过期时间: ' + data.refreshExpiresAt;
              showResult(msg, data.isExpired || data.refreshIsExpired, data.isExpired);
            } catch (e) {
              showResult('✗ 请求失败: ' + e.message, true);
            }
          }
          
          async function refreshToken() {
            try {
              showResult('正在刷新Token...');
              const res = await fetch('/refresh/token', { method: 'POST' });
              const data = await res.json();
              if (data.success) {
                showResult('✓ Token刷新成功<br>新过期时间: ' + data.expiresAt);
                setTimeout(() => location.reload(), 2000);
              } else {
                showResult('✗ ' + data.error, true);
              }
            } catch (e) {
              showResult('✗ 请求失败: ' + e.message, true);
            }
          }
        </script>
      </body>
    </html>
  `);
});

// ===================== 自动同步模块 =====================
let syncTimer = null;

function startAutoSync() {
  if (config.syncInterval > 0) {
    console.log(`✓ 启动自动同步，间隔: ${config.syncInterval}秒`);
    syncTimer = setInterval(async () => {
      try {
        await fetchAndPublishTasks();
      } catch (error) {
        console.error('自动同步出错:', error.message);
      }
    }, config.syncInterval * 1000);
  }
}

// ===================== 启动与关闭模块 =====================
function start() {
  console.log('\n=================================');
  console.log('飞书-EMQX消息桥接服务');
  console.log('=================================\n');
  
  // 尝试从文件加载token
  const loaded = loadTokensFromFile();
  
  if (!tokenStore.userAccessToken && !tokenStore.refreshToken) {
    console.error('❌ 错误: 未设置 USER_ACCESS_TOKEN 或 REFRESH_TOKEN');
    console.error('请在 .env 文件中配置:');
    console.error('  USER_ACCESS_TOKEN=你的access_token');
    console.error('  REFRESH_TOKEN=你的refresh_token');
    console.error('\n或访问飞书API Explorer获取: https://open.feishu.cn/api-explorer/');
    console.error('\n注意: 获取token时需要在scope中包含 offline_access 权限\n');
  } 
  else if (!tokenStore.refreshToken) {
    console.warn('⚠️  警告: 未设置 REFRESH_TOKEN,无法自动刷新token');
    console.warn('Token过期后需要手动重新获取\n');
  }
  
  if (!config.emqx.username || !config.emqx.password) {
    console.warn('⚠️  警告: 未设置EMQX用户名或密码');
  }
  
  // 启动定时刷新检查(每小时检查一次)
  setInterval(async () => {
    try {
      await checkAndRefreshToken();
    } catch (error) {
      console.error('定时刷新token失败:', error.message);
    }
  }, 60 * 60 * 1000); // 每小时检查
  
  connectMQTT();
  startAutoSync();
  
  app.listen(config.port, () => {
    console.log(`\n✓ HTTP服务启动成功`);
    console.log(`  访问地址: http://localhost:${config.port}`);
    console.log(`  健康检查: http://localhost:${config.port}/health`);
    console.log(`  Token状态: http://localhost:${config.port}/token/status`);
    console.log(`  MQTT主题: ${config.emqx.topic}`);
    
    if (tokenStore.userAccessToken) {
      console.log('\n💡 提示: 可以访问首页进行可视化操作');
      const now = Date.now();
      const hoursLeft = Math.floor((tokenStore.expiresAt - now) / 1000 / 3600);
      console.log(`📅 Token将在 ${hoursLeft} 小时后过期`);
    }
    
    console.log('\n=================================\n');
  });
}

process.on('SIGINT', () => {
  console.log('\n正在关闭服务...');
  if (syncTimer) clearInterval(syncTimer);
  if (mqttClient) mqttClient.end();
  process.exit(0);
});

start();
// ===================== END =====================
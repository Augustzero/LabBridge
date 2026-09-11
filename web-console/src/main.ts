import { createApp } from 'vue'
import ElementPlus from 'element-plus'
import 'element-plus/dist/index.css'

import App from './App.vue'
import { bindAuthExpiryRedirect } from './composables/useAuth'
import router from './router'
import './styles/main.css'

// 业务请求凭据失效（401）时清凭据并回输入页，装配点挂一次
bindAuthExpiryRedirect(router)

createApp(App).use(router).use(ElementPlus).mount('#app')

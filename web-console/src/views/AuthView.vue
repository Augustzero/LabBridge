<script setup lang="ts">
import { ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import { ApiError } from '@/api/http'
import { useAuth } from '@/composables/useAuth'
import { sanitizeRedirect } from '@/router'

const route = useRoute()
const router = useRouter()
const { signIn } = useAuth()

const tokenInput = ref('')
const submitting = ref(false)
const errorText = ref('')

// 验证失败要给用户可理解的提示：凭据不对、连不上服务分别说明原因
function describeRejection(err: unknown): string {
  if (err instanceof ApiError) {
    if (err.kind === 'unauthenticated') {
      return '访问凭据无效或已失效，请核对后重新输入'
    }
    if (err.kind === 'network') {
      return '无法连接控制台服务，请检查网络后重试'
    }
    if (err.kind === 'timeout') {
      return '验证请求超时，请重试'
    }
  }
  return '验证失败，请重试'
}

async function submit(): Promise<void> {
  // 粘贴的 token 可能带末尾换行或首尾空格，先裁掉再验证
  const candidate = tokenInput.value.trim()
  if (candidate === '') {
    errorText.value = '请输入访问凭据'
    return
  }
  submitting.value = true
  errorText.value = ''
  try {
    await signIn(candidate)
    // 回到进入前的目标页；redirect 来源已做应用内白名单校验
    await router.replace(sanitizeRedirect(route.query.redirect))
  } catch (err) {
    errorText.value = describeRejection(err)
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <div class="auth-view">
    <el-card shadow="never" class="auth-view__card">
      <div class="auth-view__title">LabBridge 控制台</div>
      <div class="auth-view__hint">请输入管理访问凭据后继续，凭据由部署管理员提供</div>
      <el-input
        v-model="tokenInput"
        type="password"
        show-password
        placeholder="访问凭据"
        :disabled="submitting"
        @keyup.enter="submit"
      />
      <div v-if="errorText !== ''" class="auth-view__error" role="alert">
        {{ errorText }}
      </div>
      <el-button
        type="primary"
        class="auth-view__submit"
        :loading="submitting"
        @click="submit"
      >
        验证并进入
      </el-button>
    </el-card>
  </div>
</template>

<style scoped>
.auth-view {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100vh;
  background: var(--labbridge-bg);
}

.auth-view__card {
  width: 380px;
}

.auth-view__title {
  font-size: 18px;
  font-weight: 600;
  text-align: center;
}

.auth-view__hint {
  margin: 8px 0 16px;
  color: #909399;
  font-size: 13px;
  text-align: center;
}

.auth-view__error {
  margin-top: 8px;
  color: #c45656;
  font-size: 13px;
  word-break: break-all;
}

.auth-view__submit {
  width: 100%;
  margin-top: 16px;
}
</style>

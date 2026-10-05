// 人工操作（执行一次 / 失败重试）的幂等键：一次按钮操作生成一个
// 32 位十六进制随机键，网络层重试期间复用同一个，服务端靠它把重复
// 提交归并成同一运行；点了新按钮才生成新键。
export function newIdempotencyKey(): string {
  const bytes = new Uint8Array(16)
  crypto.getRandomValues(bytes)
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')
}

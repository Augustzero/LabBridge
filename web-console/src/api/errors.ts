export class ApiError extends Error {
  constructor(
    public readonly code: string,
    message: string,
    public readonly status: number,
  ) {
    super(message)
    this.name = 'ApiError'
  }
}

export class NetworkError extends Error {
  constructor(message = '无法连接 LabBridge 服务，请稍后重试') {
    super(message)
    this.name = 'NetworkError'
  }
}

export class ProtocolError extends Error {
  constructor(message = '服务返回了无法识别的数据') {
    super(message)
    this.name = 'ProtocolError'
  }
}

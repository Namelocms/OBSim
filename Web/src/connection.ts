import type { ClientMessage, ServerMessage } from './protocol';

export type ConnectionState = 'connecting' | 'open' | 'closed';

export interface ConnectionHandlers {
  onMessage: (msg: ServerMessage) => void;
  onState: (state: ConnectionState, detail?: string) => void;
}

/* The socket, and the reconnect policy.
 *
 * Reconnects with backoff rather than giving up, because the common case is not a network
 * fault -- it is the server being restarted while the page stays open, which happens
 * constantly during development and whenever the desktop shell relaunches the engine.
 *
 * Deliberately dumb about content: it parses JSON and hands messages on. Anything that
 * knows what a frame MEANS lives elsewhere.
 */
export class Connection {
  private socket: WebSocket | null = null;
  private closedByUs = false;
  private retryMs = 250;
  private retryTimer: number | null = null;

  constructor(private url: string, private handlers: ConnectionHandlers) {}

  connect(): void {
    this.closedByUs = false;
    this.handlers.onState('connecting');

    let socket: WebSocket;
    try {
      socket = new WebSocket(this.url);
    } catch (err) {
      this.scheduleRetry(String(err));
      return;
    }
    this.socket = socket;

    socket.onopen = () => {
      this.retryMs = 250;
      this.handlers.onState('open');
    };

    socket.onmessage = (event) => {
      if (typeof event.data !== 'string') return;
      let msg: ServerMessage;
      try {
        msg = JSON.parse(event.data) as ServerMessage;
      } catch {
        // A malformed frame is not worth tearing the connection down over
        return;
      }
      this.handlers.onMessage(msg);
    };

    socket.onerror = () => {
      // onclose always follows, so the retry is scheduled there rather than twice here
    };

    socket.onclose = (event) => {
      this.socket = null;
      if (this.closedByUs) {
        this.handlers.onState('closed', 'disconnected');
        return;
      }
      this.scheduleRetry(event.reason || 'connection lost');
    };
  }

  private scheduleRetry(detail: string): void {
    this.handlers.onState('closed', detail);
    if (this.retryTimer !== null) return;
    const wait = this.retryMs;
    // Capped backoff: fast enough that a server restart is barely noticed, slow enough
    // that a server which is simply not running does not spin.
    this.retryMs = Math.min(this.retryMs * 2, 4000);
    this.retryTimer = window.setTimeout(() => {
      this.retryTimer = null;
      if (!this.closedByUs) this.connect();
    }, wait);
  }

  send(msg: ClientMessage): void {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
    this.socket.send(JSON.stringify(msg));
  }

  close(): void {
    this.closedByUs = true;
    if (this.retryTimer !== null) {
      window.clearTimeout(this.retryTimer);
      this.retryTimer = null;
    }
    this.socket?.close();
    this.socket = null;
  }
}

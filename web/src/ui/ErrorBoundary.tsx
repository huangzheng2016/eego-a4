import { Component, type ErrorInfo, type ReactNode } from 'react';

interface State {
  error: Error | null;
  stack: string;
}

/** Last-resort screen for a render crash: shows the error instead of a blank page. */
export class ErrorBoundary extends Component<{ children: ReactNode }, State> {
  state: State = { error: null, stack: '' };

  static getDerivedStateFromError(error: Error): Partial<State> {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.error('[eego] render crash:', error, info.componentStack);
    this.setState({ stack: `${error.stack ?? error.message}\n${info.componentStack ?? ''}` });
  }

  render(): ReactNode {
    const { error, stack } = this.state;
    if (!error) return this.props.children;
    return (
      <div className="crash-screen">
        <h2>页面出错 / Something went wrong</h2>
        <p className="mono">{error.message}</p>
        <pre className="mono device-log">{stack}</pre>
        <div className="row">
          <button className="btn primary" onClick={() => location.reload()}>重新加载 / Reload</button>
          <button className="btn" onClick={() => void navigator.clipboard?.writeText(`${error.message}\n${stack}`)}>复制 / Copy</button>
        </div>
      </div>
    );
  }
}

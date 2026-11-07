// Login form. Shows lockout / wrong-password errors from the backend.

import { createSignal, Show } from "solid-js";
import { ApiError } from "../api";
import { login, setupNeeded } from "../auth";
import { errorText, t } from "../i18n";
import { LangToggle, TextField } from "./Controls";
import { Icon } from "./Icon";

export function Login() {
  const [username, setUsername] = createSignal("");
  const [password, setPassword] = createSignal("");
  const [error, setError] = createSignal("");
  const [busy, setBusy] = createSignal(false);

  const submit = async (e: Event) => {
    e.preventDefault();
    if (busy() || !username() || !password()) return;
    setBusy(true);
    setError("");
    try {
      await login(username(), password());
    } catch (err) {
      setPassword("");
      setError(err instanceof ApiError ? errorText(err.code) : errorText("network"));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="login-page">
      <div class="login-lang">
        <LangToggle />
      </div>
      <form class="login-card" onSubmit={submit}>
        <div class="login-logo">
          <img src="/icon.svg" alt="" width="56" height="56" />
        </div>
        <h1 class="headline-small">{t("appName")}</h1>
        <p class="body-medium variant">{t("signInTitle")}</p>

        <Show when={setupNeeded()}>
          <div class="setup-hint body-medium">
            <strong>{t("setupTitle")}</strong>
            <span>{t("setupText")}</span>
            <code>docker compose exec -it dashboard dashboard_backend --create-admin &lt;name&gt;</code>
          </div>
        </Show>

        <TextField label={t("username")} value={username()} onInput={setUsername} autocomplete="username" autofocus />
        <TextField
          label={t("password")}
          type="password"
          value={password()}
          onInput={setPassword}
          autocomplete="current-password"
          error={!!error()}
        />
        <Show when={error()}>
          <p class="form-error body-medium" role="alert">
            <Icon name="error" size={18} />
            {error()}
          </p>
        </Show>
        <button class="filled-button label-large" type="submit" disabled={busy() || !username() || !password()}>
          <Show when={busy()} fallback={t("signIn")}>
            <span class="button-spinner" />
          </Show>
        </button>
      </form>
    </div>
  );
}

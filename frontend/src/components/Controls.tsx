// Reusable form controls: language toggle, text field, modal dialog, switch.

import { createSignal, For, Show, type JSX } from "solid-js";
import { lang, setLang, t, type Lang } from "../i18n";
import { Icon } from "./Icon";

// DE/EN button in the header.
export function LangToggle() {
  return (
    <div class="segmented label-large" role="radiogroup" aria-label={t("language")}>
      <For each={["de", "en"] as Lang[]}>
        {(l) => (
          <button role="radio" aria-checked={lang() === l} classList={{ selected: lang() === l }} onClick={() => setLang(l)}>
            {l.toUpperCase()}
          </button>
        )}
      </For>
    </div>
  );
}

/** M3 outlined text field. */
export function TextField(props: {
  label: string;
  value: string;
  onInput: (v: string) => void;
  type?: "text" | "password";
  autocomplete?: string;
  hint?: string;
  error?: boolean;
  autofocus?: boolean;
}) {
  const [visible, setVisible] = createSignal(false);
  const isPassword = () => props.type === "password";
  return (
    <label class="text-field" classList={{ error: props.error }}>
      <span class="text-field-box">
        <input
          type={isPassword() && !visible() ? "password" : "text"}
          value={props.value}
          placeholder=" "
          autocomplete={props.autocomplete}
          autocapitalize="none"
          spellcheck={false}
          autofocus={props.autofocus}
          maxlength={128}
          onInput={(e) => props.onInput(e.currentTarget.value)}
        />
        <span class="text-field-label body-small">{props.label}</span>
        <Show when={isPassword()}>
          <button type="button" class="icon-button" aria-label={t("showPassword")} onClick={() => setVisible((v) => !v)}>
            <Icon name={visible() ? "visibility_off" : "visibility"} size={20} />
          </button>
        </Show>
      </span>
      <Show when={props.hint}>
        <span class="text-field-hint body-small">{props.hint}</span>
      </Show>
    </label>
  );
}

/** M3 dialog with scrim. Closes on Escape and scrim click. */
export function Dialog(props: { title: string; onClose: () => void; children: JSX.Element; actions: JSX.Element }) {
  return (
    <div
      class="scrim"
      onClick={(e) => e.target === e.currentTarget && props.onClose()}
      onKeyDown={(e) => e.key === "Escape" && props.onClose()}
    >
      <div class="dialog" role="dialog" aria-modal="true" aria-label={props.title}>
        <h2 class="headline-small">{props.title}</h2>
        <div class="dialog-body">{props.children}</div>
        <div class="dialog-actions">{props.actions}</div>
      </div>
    </div>
  );
}

// Accessible on/off switch.
export function Switch(props: { checked: boolean; onChange: (v: boolean) => void; label: string; disabled?: boolean }) {
  return (
    <button
      type="button"
      role="switch"
      class="switch"
      aria-checked={props.checked}
      aria-label={props.label}
      disabled={props.disabled}
      onClick={() => props.onChange(!props.checked)}
    >
      <span class="switch-handle">
        <Show when={props.checked}>
          <Icon name="check" size={16} />
        </Show>
      </span>
    </button>
  );
}

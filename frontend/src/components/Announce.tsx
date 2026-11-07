// Announcement on the Echo (Alexa) via Command.announce. Needs permission "announce".

import { createSignal, For, Show } from "solid-js";
import { send } from "../connection";
import { lang, t } from "../i18n";
import { Dialog, TextField } from "./Controls";
import { Icon } from "./Icon";

// Quick phrases per language.
const QUICK = {
  de: ["Essen ist fertig!", "Kommst du bitte kurz?", "Ich bin gleich zuhause.", "Gute Nacht!"],
  en: ["Dinner is ready!", "Can you come here for a moment?", "I'll be home soon.", "Good night!"],
};
// Max text length (backend limit).
const MAX = 200;

/** Megaphone button: type a text, the Echo speaks it (with the sender's name). */
export function AnnounceButton() {
  const [open, setOpen] = createSignal(false);
  const [text, setText] = createSignal("");
  const [state, setState] = createSignal<"idle" | "sent" | "error">("idle");

  const submit = (value: string) => {
    const v = value.trim().slice(0, MAX);
    if (!v) return;
    if (!send({ announce: { text: v } })) return setState("error");
    setState("sent");
    setText("");
    setTimeout(() => {
      setOpen(false);
      setState("idle");
    }, 1200);
  };

  return (
    <>
      <button class="icon-button announce-button" aria-label={t("announce")} title={t("announce")} onClick={() => setOpen(true)}>
        <Icon name="campaign" size={22} />
      </button>
      <Show when={open()}>
        <Dialog
          title={t("announce")}
          onClose={() => (setOpen(false), setState("idle"))}
          actions={
            <>
              <button class="text-button label-large" onClick={() => (setOpen(false), setState("idle"))}>
                {t("cancel")}
              </button>
              <button class="filled-button label-large" disabled={!text().trim() || state() === "sent"} onClick={() => submit(text())}>
                <Icon name="send" size={18} />
                {t("speak")}
              </button>
            </>
          }
        >
          <p class="body-medium variant">{t("announceHint")}</p>
          <form onSubmit={(e) => (e.preventDefault(), submit(text()))}>
            <TextField label={t("announceLabel")} value={text()} onInput={(v) => setText(v.slice(0, MAX))} autocomplete="off" autofocus hint={`${text().length} / ${MAX}`} />
          </form>
          <div class="perm-chips">
            <For each={QUICK[lang()]}>
              {(q) => (
                <button class="filter-chip label-large" onClick={() => submit(q)}>
                  {q}
                </button>
              )}
            </For>
          </div>
          <Show when={state() === "sent"}>
            <p class="announce-ok body-medium" role="status">
              <Icon name="check" size={18} />
              {t("announceSent")}
            </p>
          </Show>
          <Show when={state() === "error"}>
            <p class="form-error body-medium" role="alert">
              <Icon name="error" size={18} />
              {t("err:network")}
            </p>
          </Show>
        </Dialog>
      </Show>
    </>
  );
}

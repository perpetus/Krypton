<script lang="ts">
    import type { ClassValue } from "svelte/elements";

    type Props = { height?: string; class?: ClassValue };
    let { height = "28px", class: className }: Props = $props();
</script>

<span class={["product-logo", className]} style:height aria-hidden="true">
    <svg viewBox="127.05 105.45 1011.7 1011.7" fill="none">
        <path
            stroke="currentColor"
            stroke-width="260"
            stroke-linecap="round"
            stroke-linejoin="round"
            d="M367 303v615m188-308 342 313M493 644l406-344"
        />
    </svg>
</span>

<style>
    .product-logo {
        display: inline-flex;
        color: currentColor;
        flex: 0 0 auto;
        aspect-ratio: 1;
    }
    svg {
        display: block;
        height: 100%;
        width: auto;
    }
</style>

//! Chuyển Windows VirtualKey (host gửi lên) → KeyDescriptor / TextControlCode của Ruffle.

use ruffle_core::events::{
    KeyDescriptor, KeyLocation, LogicalKey, NamedKey, PhysicalKey, TextControlCode,
};

pub const MOD_SHIFT: u32 = 1;
pub const MOD_CTRL: u32 = 2;

pub fn key_descriptor(vk: u32, mods: u32) -> KeyDescriptor {
    use NamedKey as N;
    use PhysicalKey as P;
    let shift = mods & MOD_SHIFT != 0;
    let ch = |lo: char, hi: char| LogicalKey::Character(if shift { hi } else { lo });
    let named = |n| LogicalKey::Named(n);
    let mut loc = KeyLocation::Standard;
    let (phys, logical) = match vk {
        0x08 => (P::Backspace, named(N::Backspace)),
        0x09 => (P::Tab, named(N::Tab)),
        0x0D => (P::Enter, named(N::Enter)),
        0x10 | 0xA0 => { loc = KeyLocation::Left; (P::ShiftLeft, named(N::Shift)) }
        0xA1 => { loc = KeyLocation::Right; (P::ShiftRight, named(N::Shift)) }
        0x11 | 0xA2 => { loc = KeyLocation::Left; (P::ControlLeft, named(N::Control)) }
        0xA3 => { loc = KeyLocation::Right; (P::ControlRight, named(N::Control)) }
        0x12 | 0xA4 => { loc = KeyLocation::Left; (P::AltLeft, named(N::Alt)) }
        0xA5 => { loc = KeyLocation::Right; (P::AltRight, named(N::Alt)) }
        0x13 => (P::Pause, named(N::Pause)),
        0x14 => (P::CapsLock, named(N::CapsLock)),
        0x1B => (P::Escape, named(N::Escape)),
        0x20 => (P::Space, LogicalKey::Character(' ')),
        0x21 => (P::PageUp, named(N::PageUp)),
        0x22 => (P::PageDown, named(N::PageDown)),
        0x23 => (P::End, named(N::End)),
        0x24 => (P::Home, named(N::Home)),
        0x25 => (P::ArrowLeft, named(N::ArrowLeft)),
        0x26 => (P::ArrowUp, named(N::ArrowUp)),
        0x27 => (P::ArrowRight, named(N::ArrowRight)),
        0x28 => (P::ArrowDown, named(N::ArrowDown)),
        0x2D => (P::Insert, named(N::Insert)),
        0x2E => (P::Delete, named(N::Delete)),
        0x30..=0x39 => {
            const DIG: [P; 10] = [P::Digit0, P::Digit1, P::Digit2, P::Digit3, P::Digit4,
                P::Digit5, P::Digit6, P::Digit7, P::Digit8, P::Digit9];
            const SH: [char; 10] = [')', '!', '@', '#', '$', '%', '^', '&', '*', '('];
            let i = (vk - 0x30) as usize;
            (DIG[i], ch((b'0' + i as u8) as char, SH[i]))
        }
        0x41..=0x5A => {
            const LET: [P; 26] = [P::KeyA, P::KeyB, P::KeyC, P::KeyD, P::KeyE, P::KeyF, P::KeyG,
                P::KeyH, P::KeyI, P::KeyJ, P::KeyK, P::KeyL, P::KeyM, P::KeyN, P::KeyO, P::KeyP,
                P::KeyQ, P::KeyR, P::KeyS, P::KeyT, P::KeyU, P::KeyV, P::KeyW, P::KeyX, P::KeyY,
                P::KeyZ];
            let i = (vk - 0x41) as usize;
            let lo = (b'a' + i as u8) as char;
            (LET[i], ch(lo, lo.to_ascii_uppercase()))
        }
        0x60..=0x69 => {
            const NUM: [P; 10] = [P::Numpad0, P::Numpad1, P::Numpad2, P::Numpad3, P::Numpad4,
                P::Numpad5, P::Numpad6, P::Numpad7, P::Numpad8, P::Numpad9];
            loc = KeyLocation::Numpad;
            let i = (vk - 0x60) as usize;
            (NUM[i], LogicalKey::Character((b'0' + i as u8) as char))
        }
        0x6A => { loc = KeyLocation::Numpad; (P::NumpadMultiply, LogicalKey::Character('*')) }
        0x6B => { loc = KeyLocation::Numpad; (P::NumpadAdd, LogicalKey::Character('+')) }
        0x6D => { loc = KeyLocation::Numpad; (P::NumpadSubtract, LogicalKey::Character('-')) }
        0x6E => { loc = KeyLocation::Numpad; (P::NumpadDecimal, LogicalKey::Character('.')) }
        0x6F => { loc = KeyLocation::Numpad; (P::NumpadDivide, LogicalKey::Character('/')) }
        0x70..=0x7B => {
            const F: [(P, N); 12] = [(P::F1, N::F1), (P::F2, N::F2), (P::F3, N::F3), (P::F4, N::F4),
                (P::F5, N::F5), (P::F6, N::F6), (P::F7, N::F7), (P::F8, N::F8), (P::F9, N::F9),
                (P::F10, N::F10), (P::F11, N::F11), (P::F12, N::F12)];
            let (p, n) = F[(vk - 0x70) as usize];
            (p, named(n))
        }
        0xBA => (P::Semicolon, ch(';', ':')),
        0xBB => (P::Equal, ch('=', '+')),
        0xBC => (P::Comma, ch(',', '<')),
        0xBD => (P::Minus, ch('-', '_')),
        0xBE => (P::Period, ch('.', '>')),
        0xBF => (P::Slash, ch('/', '?')),
        0xC0 => (P::Backquote, ch('`', '~')),
        0xDB => (P::BracketLeft, ch('[', '{')),
        0xDC => (P::Backslash, ch('\\', '|')),
        0xDD => (P::BracketRight, ch(']', '}')),
        0xDE => (P::Quote, ch('\'', '"')),
        _ => (P::Unknown, LogicalKey::Unknown),
    };
    KeyDescriptor { physical_key: phys, logical_key: logical, key_location: loc }
}

/// Phím điều khiển text (giống desktop/src/util.rs).
pub fn text_control(vk: u32, mods: u32) -> Option<TextControlCode> {
    use TextControlCode as T;
    let shift = mods & MOD_SHIFT != 0;
    let ctrl = mods & MOD_CTRL != 0;
    Some(match (vk, ctrl, shift) {
        (0x0D, _, _) => T::Enter,
        (0x08, true, _) => T::BackspaceWord,
        (0x08, false, _) => T::Backspace,
        (0x2E, true, _) => T::DeleteWord,
        (0x2E, false, _) => T::Delete,
        (0x41, true, _) => T::SelectAll,
        (0x43, true, _) => T::Copy,
        (0x56, true, _) => T::Paste,
        (0x58, true, _) => T::Cut,
        (0x25, false, false) => T::MoveLeft,
        (0x25, true, false) => T::MoveLeftWord,
        (0x25, false, true) => T::SelectLeft,
        (0x25, true, true) => T::SelectLeftWord,
        (0x27, false, false) => T::MoveRight,
        (0x27, true, false) => T::MoveRightWord,
        (0x27, false, true) => T::SelectRight,
        (0x27, true, true) => T::SelectRightWord,
        (0x24, false, false) => T::MoveLeftLine,
        (0x24, true, false) => T::MoveLeftDocument,
        (0x24, false, true) => T::SelectLeftLine,
        (0x24, true, true) => T::SelectLeftDocument,
        (0x23, false, false) => T::MoveRightLine,
        (0x23, true, false) => T::MoveRightDocument,
        (0x23, false, true) => T::SelectRightLine,
        (0x23, true, true) => T::SelectRightDocument,
        _ => return None,
    })
}

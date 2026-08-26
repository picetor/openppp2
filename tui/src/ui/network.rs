//! NETWORK page: TUN/NIC details matching the built-in C++ TUI.

use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::Line;
use ratatui::widgets::Paragraph;
use ratatui::Frame;

use crate::rpc::schema::{NetworkInterface, Snapshot};
use crate::ui::widgets::kv_block;

pub fn draw(frame: &mut Frame, area: Rect, snapshot: &Snapshot) {
    let network = &snapshot.network;
    let mut sections: Vec<(&str, Vec<(String, Line<'static>)>)> = Vec::new();

    if network.mode == "proxy-only" {
        sections.push(("TUNNEL", tunnel_rows(network)));
    }

    if let Some(tun) = &network.tun {
        if network.mode != "proxy-only" {
            sections.push(("TUN", interface_rows(tun, true, network)));
        }
    }
    if let Some(nic) = &network.nic {
        sections.push(("NIC", interface_rows(nic, false, network)));
    }

    if sections.is_empty() {
        frame.render_widget(Paragraph::new("waiting for network information..."), area);
        return;
    }

    let constraints: Vec<Constraint> = sections
        .iter()
        .map(|(_, rows)| Constraint::Length(rows.len() as u16 + 2))
        .collect();
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints(constraints)
        .split(area);

    for ((title, rows), chunk) in sections.into_iter().zip(chunks.iter()) {
        frame.render_widget(kv_block(title, rows), *chunk);
    }
}

fn tunnel_rows(network: &crate::rpc::schema::Network) -> Vec<(String, Line<'static>)> {
    vec![
        ("Mode".to_string(), value_line(&network.mode)),
        ("Adapter".to_string(), value_line(&network.adapter)),
        (
            "Logical IPv4".to_string(),
            value_line(&network.logical_ipv4),
        ),
        (
            "Logical IPv6".to_string(),
            value_line(&network.logical_ipv6),
        ),
        ("Tunnel DNS".to_string(), value_line(&network.tunnel_dns)),
        ("Link State".to_string(), value_line(&network.link_state)),
        ("Mux State".to_string(), value_line(&network.mux_state)),
        (
            "TCP/IP Transport".to_string(),
            value_line(&network.tcp_ip_transport),
        ),
        (
            "DNS Transport".to_string(),
            value_line(&network.dns_transport),
        ),
    ]
}

fn interface_rows(
    interface: &NetworkInterface,
    tun: bool,
    network: &crate::rpc::schema::Network,
) -> Vec<(String, Line<'static>)> {
    let mut rows = vec![
        (
            "Name".to_string(),
            Line::from(if interface.description.is_empty() {
                interface.name.clone()
            } else {
                format!("{}[{}]", interface.name, interface.description)
            }),
        ),
        ("Index".to_string(), Line::from(interface.index.to_string())),
    ];

    if !interface.id.is_empty() {
        rows.push(("Id".to_string(), Line::from(interface.id.clone())));
    }

    rows.push((
        "Interface".to_string(),
        Line::from(format!(
            "{} {} {}",
            display_value(&interface.ipv4),
            display_value(&interface.gateway),
            display_value(&interface.subnet_mask)
        )),
    ));

    if tun {
        let ipv6 = [
            if interface.ipv6_address.is_empty() {
                String::new()
            } else {
                format!("{}/64", interface.ipv6_address)
            },
            interface.ipv6_gateway.clone(),
            interface.ipv6_subnet_mask.clone(),
        ]
        .into_iter()
        .filter(|value| !value.is_empty())
        .collect::<Vec<_>>();
        if !ipv6.is_empty() {
            rows.push(("Interface IPv6".to_string(), Line::from(ipv6.join(" "))));
        }
    } else if !interface.ipv6_gateway.is_empty() {
        rows.push((
            "Interface IPv6".to_string(),
            Line::from(interface.ipv6_gateway.clone()),
        ));
    }

    if tun {
        rows.push(("Aggligator".to_string(), value_line(&network.aggligator)));
        rows.push((
            "Proxy Interlayer".to_string(),
            value_line(&network.proxy_interlayer),
        ));
        rows.push(("TCP/IP CC".to_string(), value_line(&network.tcp_ip_cc)));
        rows.push(("Block QUIC".to_string(), value_line(&network.block_quic)));
        rows.push(("Mux State".to_string(), value_line(&network.mux_state)));
        rows.push(("Link State".to_string(), value_line(&network.link_state)));
    }

    for (index, dns) in interface.dns.iter().enumerate() {
        rows.push((format!("DNS Server {}", index + 1), Line::from(dns.clone())));
    }
    rows
}

fn display_value(value: &str) -> &str {
    if value.is_empty() {
        "-"
    } else {
        value
    }
}

fn value_line(value: &str) -> Line<'static> {
    Line::from(if value.is_empty() {
        "-".to_string()
    } else {
        value.to_string()
    })
}
